#pragma once
// nostos-kit —— 命名实现竞争发布门（Tier 1）：「同一服务 id 有多个实现、按 env
// 择一首发 + set_active 事件运行期热切」的机制托管件。
//
// 竞争样板（读 env 定首发 + 订阅热切事件 + 匹配发布/不匹配撤发布 + 状态跟踪）
// 在多实现插件间逐字重复；本件收编机制，插件只留两个真正的差异点——表组装
//（on_publish / on_unpublish）与事件载荷解读（classify）。
//
// 语义：
//   · 首发：env 未设/空 ⇒ publish_when_unset 决定发布或休眠；已设 ⇒ 等于实现名
//     才发布，否则休眠。多个实现共用同一 env 名，值即当前胜出实现的名字。
//   · 热切：classify 把事件载荷解读为三值——mine（点名本实现）/ other（点名
//     别的实现）/ keep（无偏好，保持现状）；门按值驱动 publish/unpublish，并做
//     幂等（已在目标态则不动）。
//   · 日志：首发休眠、已激活、已休眠（热切）三条由门统一落（level 2，log_tag
//     前缀）；发布失败归 on_publish 自己点名（它知道服务名），热切期门再补一条
//     保持休眠的状态交代（level 3）。
//
// NOTE: 门非线程安全，也不需要——arm 与三个回调全部发生在宿主线程（activate
// 与事件派发同线程）。
// NOTE: classify 兼做「切到我」时的载荷落地（on_publish 不带载荷）——需要载荷
// 侧效果（如凭据热更）就在 classify 的 mine 分支先落，再返回 mine。
// NOTE: arm 返回的 sub 捕获门的 this，寿命不得长于门——常规形态是并入插件自己
// 的订阅清单（deactivate 统一清空退订）。

#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>

#include <nostos/abi/plugin.hpp>

namespace nostos::kit {

class active_gate {
public:
    // 热切偏好三值：mine = 事件点名本实现；other = 点名别的实现；keep = 无偏好。
    enum class prefer { keep, mine, other };

    using classify_fn = std::function<prefer(const void* payload)>;
    using publish_fn = std::function<bool()>;  // false = 发布失败（失败日志归回调点名）
    using unpublish_fn = std::function<void()>;

    active_gate(std::string log_tag, const char* env_var, std::string impl_name,
                bool publish_when_unset)
        : log_tag_(std::move(log_tag)),
          env_var_(env_var),
          impl_name_(std::move(impl_name)),
          publish_when_unset_(publish_when_unset) {}

    bool published() const { return published_; }

    // arm 的产物：sub = 热切订阅；ok = 首发收口（休眠也算成功，false 仅当首发
    // 尝试发布且失败——插件据此中止激活，半成品由宿主激活事务回滚）。
    struct armed {
        nostos::abi::Host::Subscription sub;
        bool ok = false;
    };

    armed arm(const nostos::abi::Host& host, std::uint64_t set_active_evt,
              classify_fn classify, publish_fn on_publish, unpublish_fn on_unpublish) {
        armed out;
        // 首发：env 门三态。已设但不等于本实现名、或未设且缺省极性为休眠 ⇒ 休眠；
        // 其余（未设且缺省发布 / 已设且等于本实现名）⇒ 发布。
        const char* v = std::getenv(env_var_.c_str());
        const bool env_set = v != nullptr && *v != '\0';
        if (env_set && std::string_view(v) != impl_name_) {
            host.log(2, log_tag_ + ": 休眠（" + env_var_ + "=" + v + "，可经热切激活）");
        } else if (!env_set && !publish_when_unset_) {
            host.log(2, log_tag_ + ": 休眠（" + env_var_ + " 未设，可经热切激活）");
        } else {
            if (!on_publish()) return out;  // 失败详情由 on_publish 点名
            published_ = true;
            host.log(2, log_tag_ + ": 已激活");
        }
        out.ok = true;
        out.sub = host.on(set_active_evt,
                          [this, host, classify = std::move(classify),
                           on_publish = std::move(on_publish),
                           on_unpublish = std::move(on_unpublish)](const void* p) mutable {
                              switch (classify(p)) {
                                  case prefer::keep:
                                      break;
                                  case prefer::mine:
                                      if (!published_) {
                                          if (on_publish()) {
                                              published_ = true;
                                              host.log(2, log_tag_ + ": 已激活");
                                          } else {
                                              host.log(3, log_tag_ + ": 热切发布失败，保持休眠");
                                          }
                                      }
                                      break;
                                  case prefer::other:
                                      if (published_) {
                                          on_unpublish();
                                          published_ = false;
                                          host.log(2, log_tag_ + ": 已休眠（热切）");
                                      }
                                      break;
                              }
                          });
        return out;
    }

private:
    std::string log_tag_;
    std::string env_var_;
    std::string impl_name_;
    bool publish_when_unset_;
    bool published_ = false;
};

}  // namespace nostos::kit
