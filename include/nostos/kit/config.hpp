#pragma once
// nostos-kit —— 配置服务：四层取值（右侧覆盖左侧）
//   内置默认 ← nostos.json ← 环境变量 ← 运行时覆盖
// + on_change 订阅（解析值变化才触发，回调在调用线程同步执行）。
//
// JSON 层使用 vendored nlohmann/json（Tier 1 首个准入，3.11.3，版本钉死）；
// 只在 load_file 一处使用——其余部分零依赖。
//
// 环境层的变更无通知机制（getenv 无回调），文档已注明：该层的变更不触发 on_change。

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nostos::kit {

class Config {
public:
    struct Contrib {
        std::string key;
        std::string default_value;
        std::string summary;
    };

    // ---- 贡献层（manifest 或代码）：键 + 默认值。重复声明以首次为准。--------
    void declare(const std::string& key, const std::string& default_value,
                 const std::string& summary = "") {
        std::lock_guard<std::mutex> lock(mu_);
        if (declared_.count(key)) return;
        declared_[key] = default_value;
        summaries_[key] = summary;
    }

    // ---- 文件层：JSON 对象，扁平键值（数字/布尔统一转字符串，嵌套点路径）----
    // 文件缺失/解析失败 ⇒ false（保留既有层，不抛）。
    // 变更通知：更新前对涉及的键记旧值，更新后值变了才触发 on_change。
    bool load_file(const std::string& path) {
        std::ifstream in(path);
        if (!in) return false;
        nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object()) return false;

        std::map<std::string, std::string> file_layer;
        flatten("", doc, &file_layer);

        std::map<std::string, std::string> before;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::vector<std::string> keys;
            for (auto& kv : file_layer) keys.push_back(kv.first);
            for (auto& kv : declared_) keys.push_back(kv.first);
            for (const auto& key : keys)
                if (auto v = resolved_locked(key); v) before[key] = *v;
            for (auto& kv : file_layer) file_[kv.first] = kv.second;
        }
        for (auto& kv : file_layer) {
            const std::string before_value =
                before.count(kv.first) ? before.at(kv.first) : std::string{};
            fire_if_changed(kv.first, before_value);
        }
        return true;
    }

    // ---- 环境层绑定：配置键 → 环境变量名 ------------------------------------
    // 变更无通知（getenv 无回调）：该层的变更不触发 on_change，文档已注明。
    void bind_env(const std::string& key, const std::string& env_name) {
        std::lock_guard<std::mutex> lock(mu_);
        env_[key] = env_name;
    }

    // 便捷：为声明的全部键按 "前缀 + 大写(点/横线 → 下划线)" 绑定环境变量。
    void bind_env_prefix(std::string prefix) {
        std::vector<std::string> keys;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [key, value] : declared_) keys.push_back(key);
        }
        for (const auto& key : keys) {
            std::string env = prefix;
            for (char c : key) {
                if (c == '.' || c == '-') env += '_';
                else env += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            bind_env(key, env);
        }
    }

    // ---- 覆盖层：设值（解析值变化才发 on_change）----------------------------
    void set_override(const std::string& key, const std::string& value) {
        std::string before;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (auto v = resolved_locked(key); v) before = *v;
            override_[key] = value;
        }
        fire_if_changed(key, before);
    }
    void clear_override(const std::string& key) {
        std::string before;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (auto v = resolved_locked(key); v) before = *v;
            override_.erase(key);
        }
        fire_if_changed(key, before);
    }

    // ---- 取值：override → env → file → declared ----------------------------
    bool get(const std::string& key, std::string* out) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto value = resolved_locked(key);
        if (!value) return false;
        if (out != nullptr) *out = *value;
        return true;
    }
    std::string get_or(const std::string& key, const std::string& fallback = {}) const {
        std::string value;
        return get(key, &value) ? value : fallback;
    }
    bool has(const std::string& key) const { return get(key, nullptr); }

    // ---- on_change：解析值变化时触发（回调在触发线程同步执行）---------------
    void on_change(const std::string& key, std::function<void(std::string)> fn) {
        std::lock_guard<std::mutex> lock(mu_);
        watchers_[key].push_back(std::move(fn));
    }

    std::vector<Contrib> contributions() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Contrib> out;
        for (auto& [key, def] : declared_) {
            out.push_back(Contrib{key, def,
                                  summaries_.count(key) ? summaries_.at(key) : std::string{}});
        }
        return out;
    }

private:
    // 线程内小工具：解析值（override → env → file → declared）。
    std::optional<std::string> resolved_locked(const std::string& key) const {
        if (auto it = override_.find(key); it != override_.end()) return it->second;
        if (auto it = env_.find(key); it != env_.end()) {
            const char* v = std::getenv(it->second.c_str());
            if (v != nullptr && *v != '\0') return std::string(v);
        }
        if (auto it = file_.find(key); it != file_.end()) return it->second;
        if (auto it = declared_.find(key); it != declared_.end()) return it->second;
        return std::nullopt;
    }

    void fire_if_changed(const std::string& key, const std::string& before) {
        std::string after;
        get(key, &after);
        if (after == before) return;
        std::vector<std::function<void(std::string)>> fns;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (auto it = watchers_.find(key); it != watchers_.end()) fns = it->second;
        }
        for (auto& fn : fns) fn(after);
    }

    static void flatten(const std::string& prefix, const nlohmann::json& node,
                        std::map<std::string, std::string>* out) {
        if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                const std::string child = prefix.empty() ? it.key() : prefix + "." + it.key();
                flatten(child, it.value(), out);
            }
            return;
        }
        if (node.is_string()) (*out)[prefix] = node.get<std::string>();
        else if (node.is_boolean()) (*out)[prefix] = node.get<bool>() ? "true" : "false";
        else if (node.is_number()) (*out)[prefix] = node.dump();
        else (*out)[prefix] = node.dump();  // 数组等复杂值按 JSON 文本存放
    }

    mutable std::mutex mu_;
    std::map<std::string, std::string> declared_;
    std::map<std::string, std::string> summaries_;
    std::map<std::string, std::string> file_;
    std::map<std::string, std::string> override_;
    std::map<std::string, std::string> env_;
    std::map<std::string, std::vector<std::function<void(std::string)>>> watchers_;
};

}  // namespace nostos::kit
