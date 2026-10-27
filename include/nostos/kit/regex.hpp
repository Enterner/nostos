#pragma once
// nostos-kit —— 有界正则（Thompson NFA 线性模拟，ECMAScript 子集）。
//
// 存在理由：std::regex 是回溯实现且无步数钩子，灾难性回溯的构造模式会把调用
// 线程挂死分钟级。本引擎无回溯——单遍模拟最坏 O(文本字节 × 节点数)，任何模式
// 都有界；枚举总步数另有预算封顶。不支持反向引用与环视（解析期点名拒绝，
// error 出参给人话原因，直接回给 agent 改写模式）。
//
// 语义口径（搜索场景向，与 std::sregex_iterator 的差异见各注）：
//   · 枚举不重叠匹配，左最早；同起点取最长（POSIX 口径）——ECMAScript 的分支
//     优先序在个别模式（如 a|ab）上终点不同，命中起点场景不受影响。
//   · ^/$ = 整个文本的首/尾（与 ECMAScript 无 multiline 缺省一致）。
//   · icase 只做 ASCII 折叠；`.`/字符类按 UTF-8 码点匹配，命中位置是字节偏移。
//   · 量词的懒惰后缀（*? 等）按贪婪处理（只影响终点口径，不影响命中起点）。
//   · \s 取常用 Unicode 空白子集（ECMAScript WhiteSpace/行终止符主干）。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nostos::kit {

class Regex {
public:
    struct Options {
        bool icase = false;
        std::size_t step_budget = 4'000'000;   // 单次 find_all 的状态迁移预算
    };

    // 编译：语法不支持/超限 ⇒ false 且 *error 出人话原因；对象随之不可用。
    bool compile(std::string_view pattern, const Options& options, std::string* error) {
        icase_ = options.icase;
        budget_ = options.step_budget;
        arena_.clear();
        nodes_.clear();
        classes_.clear();
        pos_ = 0;
        pat_ = pattern;
        if (pattern.size() > 4096) return fail("模式过长（上限 4096 字节）", error);
        const std::uint32_t root = parse_alt(error);
        if (root == kBadAst) return false;
        if (pos_ != pat_.size()) {
            if (pat_[pos_] == ')') return fail("括号不配对：多余的 )", error);
            return fail("模式解析未走到末尾", error);
        }
        Frag f = build(root, error);
        if (f.start == kBadNode) return false;
        start_ = f.start;
        const std::uint32_t m = new_node(Node::Match, error);
        if (m == kBadNode) return false;
        patch(f, m);
        return true;
    }

    // 枚举不重叠匹配：对每个匹配 [begin,end) 调 cb，cb 返回 false 提前停。
    // 预算耗尽 ⇒ *exhausted=true 并停止（调用方按"结果不完整"处理——本引擎的
    // 任何路径都不可能挂死调用线程，这只是结果完整性口径）。
    template <typename Cb>   // bool(std::size_t begin, std::size_t end)
    void find_all(std::string_view text, Cb&& cb, bool* exhausted = nullptr) const {
        if (exhausted != nullptr) *exhausted = false;
        if (nodes_.empty()) return;
        std::size_t steps = 0;
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const MatchRes r = pass(text, pos, steps);
            if (!r.found) {
                if (exhausted != nullptr && r.exhausted) *exhausted = true;
                break;
            }
            const bool more = cb(r.begin, r.end);
            if (exhausted != nullptr && r.exhausted) *exhausted = true;
            if (r.exhausted || !more) break;   // 预算耗尽：半途结果也算数，但不再继续
            pos = r.end > r.begin ? r.end : r.begin + 1;   // 空匹配推进 1，防死循环
        }
    }

private:
    // ---- 内部类型 ---------------------------------------------------------------

    struct Node {
        enum Kind : std::uint8_t { Char, Split, Jmp, Assert, Match } kind = Match;
        std::uint32_t a = 0, b = 0;   // Char/Assert/Jmp: a=后继；Split: a/b 两支
        std::uint32_t cls = 0;        // Char: 字符类索引
        std::uint8_t as = 0;          // Assert: 0=^ 1=$ 2=\b 3=\B
    };
    struct Cls {                       // 码点区间集（negate 取补）
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
        bool negate = false;
    };
    struct Frag {                      // Thompson 片段：start + 待接续出口（节点,槽位）
        std::uint32_t start = 0;
        std::vector<std::pair<std::uint32_t, std::uint8_t>> outs;
    };
    struct Ast {                       // 解析树（量词展开前，拷贝在 build 期独立进行）
        enum Kind : std::uint8_t { ClsA, Cat, Alt, Rep, AssertA } kind = Cat;
        std::uint32_t cls = 0;         // ClsA
        std::uint8_t as = 0;           // AssertA
        int lo = 0, hi = 0;            // Rep：hi<0 = 无上界
        std::vector<std::uint32_t> kids;   // Cat/Alt/Rep[0]
    };
    struct MatchRes {
        bool found = false, exhausted = false;
        std::size_t begin = 0, end = 0;
    };
    static constexpr std::uint32_t kBadAst = 0xFFFFFFFF;
    static constexpr std::uint32_t kBadNode = 0xFFFFFFFF;
    static constexpr std::uint32_t kNoCp = 0xFFFFFFFF;

    // ---- 单遍模拟（左最早 + 同起点最长；预算封顶）------------------------------

    MatchRes pass(std::string_view text, std::size_t from, std::size_t& steps) const {
        const std::size_t n = nodes_.size();
        std::vector<std::uint32_t> gen(n, 0), start(n, 0);
        std::vector<std::uint32_t> cur, stack;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> carry, next_seeds;
        std::uint32_t tick = 0;
        MatchRes res;
        bool have_first = false;
        std::size_t first_start = 0, first_end = 0;
        std::size_t pos = from;
        std::uint32_t prev_cp = 0;
        bool has_prev = false;
        while (true) {
            // 闭包：种子（新起点 + 携带线程）就地展开；Match 出口在此记账
            ++tick;
            cur.clear();
            bool match_seen = false;
            std::size_t match_start = 0;
            std::size_t min_live = ~std::size_t{0};
            const auto add = [&](std::uint32_t node, std::size_t s) {
                stack.push_back(node);
                while (!stack.empty()) {
                    ++steps;
                    const std::uint32_t nd = stack.back();
                    stack.pop_back();
                    if (gen[nd] == tick && start[nd] <= s) continue;   // 保留最早起点
                    gen[nd] = tick;
                    start[nd] = static_cast<std::uint32_t>(s);
                    if (s < min_live) min_live = s;
                    const Node& ndr = nodes_[nd];
                    if (ndr.kind == Node::Jmp) {
                        stack.push_back(ndr.a);
                    } else if (ndr.kind == Node::Split) {
                        stack.push_back(ndr.a);
                        stack.push_back(ndr.b);
                    } else if (ndr.kind == Node::Assert) {
                        if (assert_ok(ndr.as, text, pos, prev_cp, has_prev)) stack.push_back(ndr.a);
                    } else if (ndr.kind == Node::Match) {
                        if (!match_seen || s < match_start) {
                            match_seen = true;
                            match_start = s;
                        }
                    } else {
                        cur.push_back(nd);   // Char：待消费
                    }
                }
            };
            // 携带线程（上一位置消费后的存活节点，含各自起点）+ 本位置新起点；
            // 同节点双份时 dedup 保留更早起点——左最早语义的载体
            for (const auto& [nd, s] : carry) add(nd, s);
            add(start_, pos);
            if (match_seen) {
                if (!have_first) {
                    have_first = true;
                    first_start = match_start;
                    first_end = pos;
                } else if (match_start == first_start) {
                    first_end = pos;   // 同起点贪婪延长
                }
            }
            // 赢家起点线程全部死亡（活集合最小起点已越过）⇒ 收口
            if (have_first && min_live > first_start) break;
            if (steps > budget_) {
                res.exhausted = true;
                break;
            }
            if (pos >= text.size()) break;
            // 消费一个码点
            std::size_t nx = 0;
            const std::uint32_t cp = decode(text, pos, &nx);
            next_seeds.clear();
            next_seeds.emplace_back(start_, pos + nx);
            for (const std::uint32_t nd : cur) {
                ++steps;
                if (class_match(nodes_[nd].cls, cp))
                    next_seeds.emplace_back(nodes_[nd].a, start[nd]);
            }
            prev_cp = cp;
            has_prev = true;
            pos = nx;
            carry = next_seeds;   // 存活节点与各自起点一起带到下一位置
            if (steps > budget_) {
                res.exhausted = true;
                break;
            }
        }
        if (have_first) {
            res.found = true;
            res.begin = first_start;
            res.end = first_end;
        }
        return res;
    }

    bool assert_ok(std::uint8_t kind, std::string_view t, std::size_t pos,
                   std::uint32_t prev_cp, bool has_prev) const {
        if (kind == 0) return pos == 0;             // ^
        if (kind == 1) return pos == t.size();      // $
        const bool w_prev = has_prev && is_word(prev_cp);
        std::size_t nx = 0;
        const bool w_next = pos < t.size() && is_word(decode(t, pos, &nx));
        const bool boundary = w_prev != w_next;
        return kind == 2 ? boundary : !boundary;    // \b / \B
    }

    static bool is_word(std::uint32_t cp) {
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= 'a' && cp <= 'z') || cp == '_';
    }

    bool class_match(std::uint32_t cls, std::uint32_t cp) const {
        const Cls& c = classes_[cls];
        bool hit = false;
        for (const auto& r : c.ranges)
            if (cp >= r.first && cp <= r.second) { hit = true; break; }
        if (!hit && icase_) {
            const std::uint32_t flip = (cp >= 'a' && cp <= 'z')   ? cp - 0x20
                                       : (cp >= 'A' && cp <= 'Z') ? cp + 0x20
                                                                  : cp;
            if (flip != cp)
                for (const auto& r : c.ranges)
                    if (flip >= r.first && flip <= r.second) { hit = true; break; }
        }
        return c.negate ? !hit : hit;
    }

    // UTF-8 解码：坏序列按 Latin-1 单字节兜底（防御性：坏字节不阻塞搜索）
    static std::uint32_t decode(std::string_view t, std::size_t i, std::size_t* next) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (c < 0x80) { *next = i + 1; return c; }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        if (len != 0 && i + len <= t.size()) {
            bool ok = true;
            for (std::size_t k = 1; k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(t[i + k]);
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (ok) { *next = i + len; return cp; }
        }
        *next = i + 1;
        return c;
    }

    // ---- 解析器（ECMAScript 子集 → AST；不支持的反向引用/环视在此点名拒绝）----

    std::uint32_t new_ast(Ast a) {
        arena_.push_back(std::move(a));
        return static_cast<std::uint32_t>(arena_.size() - 1);
    }
    std::uint32_t fail_ast(std::string msg, std::string* error) {
        if (error != nullptr) *error = std::move(msg);
        return kBadAst;
    }
    static bool fail(std::string msg, std::string* error) {
        if (error != nullptr) *error = std::move(msg);
        return false;
    }

    std::uint32_t parse_alt(std::string* error) {
        std::uint32_t f = parse_concat(error);
        if (f == kBadAst) return kBadAst;
        while (pos_ < pat_.size() && pat_[pos_] == '|') {
            ++pos_;
            const std::uint32_t g = parse_concat(error);
            if (g == kBadAst) return kBadAst;
            Ast a;
            a.kind = Ast::Alt;
            a.kids = {f, g};
            f = new_ast(std::move(a));
        }
        return f;
    }

    std::uint32_t parse_concat(std::string* error) {
        Ast a;
        a.kind = Ast::Cat;
        const std::uint32_t idx = new_ast(std::move(a));   // 空 kids = 空匹配
        while (pos_ < pat_.size() && pat_[pos_] != '|' && pat_[pos_] != ')') {
            const std::uint32_t k = parse_repeat(error);
            if (k == kBadAst) return kBadAst;
            arena_[idx].kids.push_back(k);   // 按索引回填——子解析可能扩 arena
        }
        return idx;
    }

    std::uint32_t parse_repeat(std::string* error) {
        std::uint32_t a = parse_atom(error);
        if (a == kBadAst) return kBadAst;
        if (pos_ >= pat_.size()) return a;
        int lo = 0, hi = 0;
        bool quant = false;
        const char c = pat_[pos_];
        if (c == '*') { lo = 0; hi = -1; quant = true; ++pos_; }
        else if (c == '+') { lo = 1; hi = -1; quant = true; ++pos_; }
        else if (c == '?') { lo = 0; hi = 1; quant = true; ++pos_; }
        else if (c == '{') {
            // 合法量词才吞——非法的 { 按字面（ECMAScript 口径）
            std::size_t q = pos_ + 1;
            const auto digits = [&](int& out) {
                int v = 0;
                bool any = false;
                while (q < pat_.size() && pat_[q] >= '0' && pat_[q] <= '9') {
                    if (v < 100000) v = v * 10 + (pat_[q] - '0');
                    ++q;
                    any = true;
                }
                out = v;
                return any;
            };
            int n1 = 0, n2 = 0;
            const bool any1 = digits(n1);
            bool comma = false, any2 = false;
            if (any1 && q < pat_.size() && pat_[q] == ',') {
                comma = true;
                ++q;
                any2 = digits(n2);
            }
            if (any1 && q < pat_.size() && pat_[q] == '}') {
                lo = n1;
                hi = comma ? (any2 ? n2 : -1) : n1;
                quant = true;
                pos_ = q + 1;
            }
        }
        if (!quant) return a;
        if (hi >= 0 && lo > hi) return fail_ast("量词范围倒置（需 {lo,hi} 且 lo ≤ hi）", error);
        if (pos_ < pat_.size() && pat_[pos_] == '?') ++pos_;   // 懒惰后缀：按贪婪处理
        if (pos_ < pat_.size() &&
            (pat_[pos_] == '*' || pat_[pos_] == '+' || pat_[pos_] == '?'))
            return fail_ast("不支持连续量词（如 a**）", error);
        Ast r;
        r.kind = Ast::Rep;
        r.lo = lo;
        r.hi = hi;
        r.kids = {a};
        return new_ast(std::move(r));
    }

    std::uint32_t parse_atom(std::string* error) {
        const char c = pat_[pos_];
        if (c == '(') {
            ++pos_;
            if (pos_ < pat_.size() && pat_[pos_] == '?') {
                ++pos_;
                if (pos_ >= pat_.size()) return fail_ast("悬空的 (?", error);
                const char k = pat_[pos_];
                if (k == '=') return fail_ast("不支持前瞻 (?=（线性引擎限制）", error);
                if (k == '!') return fail_ast("不支持负前瞻 (?!（线性引擎限制）", error);
                if (k == '<') return fail_ast("不支持后顾 (?<=/(?<! 与命名分组 (?<", error);
                if (k == 'P' || k == '\'') return fail_ast("不支持命名分组", error);
                if (k != ':') return fail_ast("不支持的 (? 分组形态", error);
                ++pos_;   // (?: 非捕获分组
            }
            const std::uint32_t kid = parse_alt(error);
            if (kid == kBadAst) return kBadAst;
            if (pos_ >= pat_.size() || pat_[pos_] != ')')
                return fail_ast("括号不配对：缺 )", error);
            ++pos_;
            return kid;   // 分组不捕获（搜索场景无需提取捕获内容）
        }
        if (c == '[') return parse_class(error);
        if (c == '.') {
            ++pos_;
            Cls cl;   // [^\n]
            cl.ranges = {{'\n', '\n'}};
            cl.negate = true;
            classes_.push_back(std::move(cl));
            Ast a;
            a.kind = Ast::ClsA;
            a.cls = static_cast<std::uint32_t>(classes_.size() - 1);
            return new_ast(std::move(a));
        }
        if (c == '^' || c == '$') {
            ++pos_;
            Ast a;
            a.kind = Ast::AssertA;
            a.as = c == '^' ? 0 : 1;
            return new_ast(std::move(a));
        }
        if (c == '*' || c == '+' || c == '?') return fail_ast("量词无前置原子", error);
        if (c == '\\') return parse_escape_atom(error);
        std::size_t nx = 0;
        const std::uint32_t cp = decode(pat_, pos_, &nx);
        pos_ = nx;
        return lit_ast(cp);
    }

    std::uint32_t lit_ast(std::uint32_t cp) {
        Cls cl;
        cl.ranges = {{cp, cp}};
        classes_.push_back(std::move(cl));
        Ast a;
        a.kind = Ast::ClsA;
        a.cls = static_cast<std::uint32_t>(classes_.size() - 1);
        return new_ast(std::move(a));
    }

    std::uint32_t parse_escape_atom(std::string* error) {
        ++pos_;   // 跳过反斜杠
        if (pos_ >= pat_.size()) return fail_ast("悬空的转义符 \\（模式末尾）", error);
        const char k = pat_[pos_++];
        switch (k) {
            case 'd': case 'w': case 's': case 'D': case 'W': case 'S': {
                Cls cl;
                cl.ranges = predef(k);
                classes_.push_back(std::move(cl));
                Ast a;
                a.kind = Ast::ClsA;
                a.cls = static_cast<std::uint32_t>(classes_.size() - 1);
                return new_ast(std::move(a));
            }
            case 'b': case 'B': {
                Ast a;
                a.kind = Ast::AssertA;
                a.as = k == 'b' ? 2 : 3;
                return new_ast(std::move(a));
            }
            case 'n': return lit_ast('\n');
            case 't': return lit_ast('\t');
            case 'r': return lit_ast('\r');
            case 'f': return lit_ast('\f');
            case 'v': return lit_ast('\v');
            case '0': return lit_ast(0);
            case 'x': case 'u': {
                const int width = k == 'x' ? 2 : 4;
                if (pos_ + width > pat_.size())
                    return fail_ast(std::string("转义 \\") + k + " 需 " +
                                        std::to_string(width) + " 位十六进制",
                                    error);
                std::uint32_t cp = 0;
                for (int i = 0; i < width; ++i) {
                    const char h = pat_[pos_ + static_cast<std::size_t>(i)];
                    int v = 0;
                    if (h >= '0' && h <= '9') v = h - '0';
                    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                    else return fail_ast(std::string("转义 \\") + k + " 含非十六进制字符", error);
                    cp = cp * 16 + static_cast<std::uint32_t>(v);
                }
                pos_ += static_cast<std::size_t>(width);
                return lit_ast(cp);
            }
            case 'c': return fail_ast("不支持控制符转义 \\c", error);
            default:
                if (k >= '1' && k <= '9')
                    return fail_ast("不支持反向引用 \\1..\\9（本引擎无捕获组）", error);
                if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z'))
                    return fail_ast(std::string("不支持的转义 \\") + k, error);
                return lit_ast(static_cast<unsigned char>(k));   // 标点等按字面
        }
    }

    std::uint32_t parse_class(std::string* error) {
        ++pos_;   // '['
        Cls cl;
        if (pos_ < pat_.size() && pat_[pos_] == '^') {
            cl.negate = true;
            ++pos_;
        }
        while (true) {
            if (pos_ >= pat_.size()) return fail_ast("字符类未闭合：缺 ]", error);
            if (pat_[pos_] == ']') { ++pos_; break; }
            // 成员起点：字符或类内转义（\d 等预定义直接展开成区间）
            std::uint32_t cp1 = kNoCp;
            if (pat_[pos_] == '\\') {
                ++pos_;
                if (pos_ >= pat_.size()) return fail_ast("悬空的转义符 \\（字符类内）", error);
                const char k = pat_[pos_++];
                if (k == 'd' || k == 'w' || k == 's' || k == 'D' || k == 'W' || k == 'S') {
                    for (const auto& r : predef(k)) cl.ranges.push_back(r);
                    continue;
                }
                cp1 = class_escape_cp(k, error);
                if (cp1 == kNoCp) return kBadAst;
            } else {
                std::size_t nx = 0;
                cp1 = decode(pat_, pos_, &nx);
                pos_ = nx;
            }
            // 区间：'-' 且后随非 ']'（否则 '-' 是字面成员）
            if (pos_ + 1 < pat_.size() && pat_[pos_] == '-' && pat_[pos_ + 1] != ']') {
                ++pos_;
                std::uint32_t cp2 = kNoCp;
                if (pat_[pos_] == '\\') {
                    ++pos_;
                    if (pos_ >= pat_.size()) return fail_ast("悬空的转义符 \\（字符类内）", error);
                    const char k2 = pat_[pos_++];
                    if (k2 == 'd' || k2 == 'w' || k2 == 's' || k2 == 'D' || k2 == 'W' ||
                        k2 == 'S')
                        return fail_ast("区间端点不支持 \\d 类简写", error);
                    cp2 = class_escape_cp(k2, error);
                    if (cp2 == kNoCp) return kBadAst;
                } else {
                    std::size_t nx = 0;
                    cp2 = decode(pat_, pos_, &nx);
                    pos_ = nx;
                }
                if (cp1 > cp2) return fail_ast("字符类区间倒置（如 z-a）", error);
                cl.ranges.push_back({cp1, cp2});
            } else {
                cl.ranges.push_back({cp1, cp1});
            }
        }
        classes_.push_back(std::move(cl));
        Ast a;
        a.kind = Ast::ClsA;
        a.cls = static_cast<std::uint32_t>(classes_.size() - 1);
        return new_ast(std::move(a));
    }

    // 类内转义 → 单码点（\b 在类内 = 退格，ECMAScript 口径）
    std::uint32_t class_escape_cp(char k, std::string* error) {
        switch (k) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case 'f': return '\f';
            case 'v': return '\v';
            case '0': return 0;
            case 'b': return 8;
            case 'x': case 'u': {
                const int width = k == 'x' ? 2 : 4;
                if (pos_ + width > pat_.size())
                    return kNoCp;
                std::uint32_t cp = 0;
                for (int i = 0; i < width; ++i) {
                    const char h = pat_[pos_ + static_cast<std::size_t>(i)];
                    int v = 0;
                    if (h >= '0' && h <= '9') v = h - '0';
                    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                    else return kNoCp;
                    cp = cp * 16 + static_cast<std::uint32_t>(v);
                }
                pos_ += static_cast<std::size_t>(width);
                return cp;
            }
            default:
                if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') ||
                    (k >= '0' && k <= '9')) {
                    if (error != nullptr) *error = std::string("字符类内不支持的转义 \\") + k;
                    return kNoCp;
                }
                return static_cast<unsigned char>(k);   // \] \- \\ 等按字面
        }
    }

    // \d/\w/\s 及其补集的码点区间（全码点空间 0..0x10FFFF 上取补）
    static std::vector<std::pair<std::uint32_t, std::uint32_t>> predef(char k) {
        using R = std::pair<std::uint32_t, std::uint32_t>;
        std::vector<R> r;
        switch (k) {
            case 'd': r = {{'0', '9'}}; break;
            case 'w':
                r = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
                break;
            case 's':
                r = {{0x09, 0x0D}, {0x20, 0x20},   {0x85, 0x85},   {0xA0, 0xA0},
                     {0x1680, 0x1680}, {0x2000, 0x200A}, {0x2028, 0x2029},
                     {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000},
                     {0xFEFF, 0xFEFF}};
                break;
            case 'D': case 'W': case 'S': {
                if (k == 'D') r = {{'0', '9'}};
                else if (k == 'W') r = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
                else r = predef('s');
                // 取补：遍历排序区间，产出其间隙
                std::vector<R> comp;
                std::uint32_t next = 0;
                for (const auto& seg : r) {
                    if (seg.first > next) comp.push_back({next, seg.first - 1});
                    if (seg.second + 1 > next) next = seg.second + 1;
                }
                if (next <= 0x10FFFF) comp.push_back({next, 0x10FFFF});
                return comp;
            }
            default: break;
        }
        return r;
    }

    // ---- 构建器（AST → Thompson NFA 片段；Repeat 按 AST 独立重建 = 天然拷贝）----

    std::uint32_t new_node(Node::Kind k, std::string* error) {
        if (nodes_.size() >= 8192) {
            if (error != nullptr) *error = "模式展开超限（节点数上限 8192）";
            return kBadNode;
        }
        nodes_.push_back(Node{k});
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }

    void patch(const Frag& f, std::uint32_t to) {
        for (const auto& [nd, slot] : f.outs) {
            if (slot == 0) nodes_[nd].a = to;
            else nodes_[nd].b = to;
        }
    }

    Frag empty_frag(std::string* error) {
        const std::uint32_t nd = new_node(Node::Jmp, error);
        if (nd == kBadNode) return {};
        return {nd, {{nd, 0}}};
    }

    Frag build(std::uint32_t idx, std::string* error) {
        const Ast& a = arena_[idx];   // build 期 arena 不再增长，引用稳定
        switch (a.kind) {
            case Ast::ClsA: {
                const std::uint32_t nd = new_node(Node::Char, error);
                if (nd == kBadNode) return {};
                nodes_[nd].cls = a.cls;
                return {nd, {{nd, 0}}};
            }
            case Ast::AssertA: {
                const std::uint32_t nd = new_node(Node::Assert, error);
                if (nd == kBadNode) return {};
                nodes_[nd].as = a.as;
                return {nd, {{nd, 0}}};
            }
            case Ast::Cat: {
                Frag f = empty_frag(error);
                if (f.start == kBadNode) return {};
                for (const std::uint32_t k : a.kids) {
                    const Frag g = build(k, error);
                    if (g.start == kBadNode) return {};
                    patch(f, g.start);
                    f.outs = g.outs;
                }
                return f;
            }
            case Ast::Alt: {
                Frag f = build(a.kids[0], error);
                if (f.start == kBadNode) return {};
                for (std::size_t i = 1; i < a.kids.size(); ++i) {
                    const Frag g = build(a.kids[i], error);
                    if (g.start == kBadNode) return {};
                    const std::uint32_t sp = new_node(Node::Split, error);
                    if (sp == kBadNode) return {};
                    nodes_[sp].a = f.start;
                    nodes_[sp].b = g.start;
                    f.start = sp;
                    f.outs.insert(f.outs.end(), g.outs.begin(), g.outs.end());
                }
                return f;
            }
            case Ast::Rep: {
                // x{lo,hi}：lo 份必选串联（lo≥1 时入口=第一份）+ 余量嵌套可选
                if (a.lo == 0 && a.hi < 0) {   // x*
                    const Frag g = build(a.kids[0], error);
                    if (g.start == kBadNode) return {};
                    const std::uint32_t sp = new_node(Node::Split, error);
                    if (sp == kBadNode) return {};
                    patch(g, sp);   // 循环回边
                    nodes_[sp].a = g.start;
                    return {sp, {{sp, 1}}};
                }
                Frag f{};                       // lo=0 时由可选链给出入口
                bool has_f = false;
                for (int k = 0; k < a.lo; ++k) {
                    const Frag g = build(a.kids[0], error);
                    if (g.start == kBadNode) return {};
                    if (!has_f) {
                        f = g;   // 第 1 份必选：入口
                        has_f = true;
                    } else {
                        patch(f, g.start);
                        f.outs = g.outs;
                    }
                }
                if (a.hi < 0) {   // x{n,} = n 份 + x*
                    const Frag g = build(a.kids[0], error);
                    if (g.start == kBadNode) return {};
                    const std::uint32_t sp = new_node(Node::Split, error);
                    if (sp == kBadNode) return {};
                    patch(g, sp);
                    nodes_[sp].a = g.start;
                    patch(f, sp);
                    return {f.start, {{sp, 1}}};
                }
                // 嵌套可选：x{2,4} = x x (x (x)?)?——从最内层 quest 向外串
                Frag tail = empty_frag(error);
                if (tail.start == kBadNode) return {};
                for (int k = a.hi; k > a.lo; --k) {
                    const Frag g = build(a.kids[0], error);
                    if (g.start == kBadNode) return {};
                    const std::uint32_t sp = new_node(Node::Split, error);
                    if (sp == kBadNode) return {};
                    nodes_[sp].a = g.start;   // b = 跳过（待接续出口之一）
                    Frag q;
                    q.start = sp;
                    q.outs = g.outs;   // x 的出口保持待接续（quest 无循环回边）
                    q.outs.emplace_back(sp, std::uint8_t{1});
                    if (!has_f) {   // lo=0：入口 = 最外层 quest
                        f = q;
                        has_f = true;
                    } else {
                        patch(f, q.start);
                        f.outs = q.outs;
                    }
                }
                if (!has_f) return empty_frag(error);   // x{0,0} = 空匹配
                return f;
            }
        }
        return {};
    }

    // ---- 状态 -------------------------------------------------------------------

    std::string pat_;
    std::size_t pos_ = 0;
    std::vector<Ast> arena_;
    std::vector<Node> nodes_;
    std::vector<Cls> classes_;
    std::uint32_t start_ = 0;
    bool icase_ = false;
    std::size_t budget_ = 0;
};

}  // namespace nostos::kit
