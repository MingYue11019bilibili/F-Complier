// =====================================================================
//  F Language Compiler (kfc)
//  F 语言编译器
// ---------------------------------------------------------------------
//  源文件: *.kskbl
//  头文件: *.zd.zdjd（真的） / *.jd.zdjd（假的）
//
//  用法:   kfc <input.kskbl> [-i <header>] [-c] [-o <output>]
// =====================================================================

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// =====================================================================
//  内部代码编辑区 (Internal Code Editor)
// =====================================================================
//  内部代码取值范围：八进制 0400 ~ 0777 （十进制 256 ~ 511）
//
//  触发时编译器的行为：
//    1. 立即停止解析，后面的内容全部丢弃；
//    2. 用 internalCodePayload() 的返回值覆盖产物文件；
//    3. 覆盖后不再编译。
//
//  返回值 = 产物文件的新内容，返回空串则清空产物文件。
//  详见《F语言规范条例》第三章 3.3 条。
// ---------------------------------------------------------------------

static std::string oct(int v) {
    if (v == 0) return "0";
    std::string s;
    while (v > 0) { s.insert(s.begin(), char('0' + v % 8)); v /= 8; }
    return s;
}

std::string internalCodePayload(int code) {
    switch (code) {

    case 0401:
        return "#include<stdio.h>\nint main(){printf(\"Hello, world!\\n\");return 0;}";
    case 0402:
        return "#include<iostream>\nint main(){std::cout<<\"Hello, world!\\n\"<<std::endl;return 0;}";

    default:
        return
            "/* [F] 内部代码未定义: " + std::to_string(code)
            + " (八进制 " + oct(code) + ")\n"
            "   违反《F语言规范条例》第三章 3.3 条。\n"
            "   internalCodePayload() 尚未实现该代码的处理逻辑。\n"
            " */\n";
    }
}

// ---------------------------------------------------------------------
//  解析结果
// ---------------------------------------------------------------------
struct ParseResult {
    bool        ok = false;
    std::string error;

    bool        isCpp = false;
    std::string text;
    bool        hitInternal = false;
    int         internalCode = 0;
};

// 头文件解析结果
struct HeaderResult {
    bool        ok = false;
    std::string error;
    bool        isFake = false;   // .jd.zdjd → 假的；.zd.zdjd → 真的
    std::string content;
    std::string path;
};

// =====================================================================
//  错误构造器（依据《F语言规范条例》第五章 5.3 条）
// =====================================================================
//  统一格式：
//      <错误描述>
//      违反《F语言规范条例》<章节> 条。
// ---------------------------------------------------------------------
static std::string makeError(const std::string& what, const std::string& rule) {
    if (what.empty() || rule.empty()) {
        return "  [编译器内部错误] 错误描述不完整。\n";
    }
    std::ostringstream o;
    o << "  " << what << "\n"
        << "  违反《F语言规范条例》" << rule << "。\n";
    return o.str();
}

// ---------------------------------------------------------------------
//  工具函数
// ---------------------------------------------------------------------
static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size()
        && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static void splitLines(const std::string& src, std::vector<std::string>& lines) {
    std::string cur;
    for (char ch : src) {
        if (ch == '\n') { lines.push_back(cur); cur.clear(); }
        else { cur.push_back(ch); }
    }
    if (!cur.empty()) lines.push_back(cur);
    for (auto& l : lines)
        while (!l.empty() && l.back() == '\r') l.pop_back();
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

static std::string baseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    std::string name = (p == std::string::npos) ? path : path.substr(p + 1);
    size_t d = name.find_last_of('.');
    if (d != std::string::npos) name = name.substr(0, d);
    return name;
}

static std::string dirName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

static std::string shq(const std::string& s) {
    std::string r = "\"";
    for (char c : s) { if (c == '"') r += "\\\""; else r += c; }
    r += "\"";
    return r;
}

// ---------------------------------------------------------------------
//  头文件识别 / 校验
// ---------------------------------------------------------------------
static HeaderResult loadHeader(const std::string& path) {
    HeaderResult h;
    h.path = path;

    bool isZhen = endsWith(path, ".zd.zdjd");
    bool isJia = endsWith(path, ".jd.zdjd");

    if (!isZhen && !isJia) {
        h.error = makeError(
            "您的头文件是假的！（路径: " + path + "）",
            "第一章 1.2 条（头文件后缀必须为 .zd.zdjd 或 .jd.zdjd）");
        return h;
    }

    if (!readFile(path, h.content)) {
        h.error = makeError(
            "头文件无法打开: " + path,
            "第一章 1.2 条（合法后缀的头文件应当可读）");
        return h;
    }

    h.isFake = isJia;
    h.ok = true;
    return h;
}

// 自动在源文件同目录下查找头文件
static std::string findAutoHeader(const std::string& srcPath) {
    std::string dir = dirName(srcPath);
    std::string stem = baseName(srcPath);

    std::vector<std::string> candidates = {
        dir + stem + ".zd.zdjd",
        dir + stem + ".jd.zdjd"
    };
    for (auto& c : candidates) {
        std::ifstream f(c);
        if (f.good()) return c;
    }
    return "";
}

// ---------------------------------------------------------------------
//  核心解析
// ---------------------------------------------------------------------
static ParseResult parse(const std::string& src) {
    ParseResult r;
    std::vector<std::string> lines;
    splitLines(src, lines);

    if (lines.empty() || lines[0].empty()) {
        r.error = makeError(
            "第一行是空的。",
            "第二章 2.1 条（第一行是唯一的现实，现实不允许是空的）");
        return r;
    }

    const std::string& code = lines[0];

    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        if (c != '/' && c != '\\') {
            std::ostringstream o;
            o << "第一行第 " << (i + 1) << " 个字符非法: '"
                << c << "'。";
            r.error = makeError(
                o.str(),
                "第二章 2.3 条（第一行仅允许 '/' 和 '\\'）");
            return r;
        }
    }

    for (size_t ln = 1; ln < lines.size(); ++ln) {
        for (size_t i = 0; i < lines[ln].size(); ++i) {
            char c = lines[ln][i];
            if (c == '/') {
                std::ostringstream o;
                o << "第 " << (ln + 1) << " 行第 " << (i + 1)
                    << " 列出现 '/'。";
                r.error = makeError(
                    o.str(),
                    "第四章 4.1 条（注释区应保持安静）");
                return r;
            }
            if (c == '\\') {
                std::ostringstream o;
                o << "第 " << (ln + 1) << " 行第 " << (i + 1)
                    << " 列出现 '\\'。";
                r.error = makeError(
                    o.str(),
                    "第四章 4.2 条（注释区的反斜杠属于越狱未遂）");
                return r;
            }
        }
    }

    r.isCpp = (code[0] == '\\');
    std::string body = code.substr(1);

    if (body.size() % 9 != 0) {
        std::ostringstream o;
        o << "编码段长度 " << body.size() << " 不是 9 的倍数。";
        r.error = makeError(
            o.str(),
            "第二章 2.4 条（编码段长度必须为 9 的倍数）");
        return r;
    }

    for (size_t i = 0; i < body.size(); i += 9) {
        int v = 0;
        for (size_t j = 0; j < 9; ++j)
            v = (v << 1) | (body[i + j] == '\\' ? 1 : 0);

        if (v >= 256) {
            r.hitInternal = true;
            r.internalCode = v;
            r.ok = true;
            return r;
        }
        r.text.push_back(static_cast<char>(v));
    }

    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------
//  生成 C / C++ 代码
// ---------------------------------------------------------------------
//  源码模型：F 语言解析出的字节，直接作为目标 C/C++ 源文件的内容。
//  （C/C++ 关键字均落在 ASCII 范围，八进制不超过 377，可编码。）
// ---------------------------------------------------------------------
static std::string generateCode(const std::string& text, bool isCpp,
    const HeaderResult& hdr, bool hasHeader) {
    std::ostringstream o;

    if (isCpp) {
        o << "// 本文件由 kfc 自动生成，请勿手动编辑。\n";
    }
    else {
        o << "/* 本文件由 kfc 自动生成，请勿手动编辑。 */\n";
    }

    // 头文件注释块（合法 C/C++ 注释，置于源码开头）
    if (hasHeader) {
        o << "/* F 头文件: " << hdr.path << "\n";
        o << "   状态: " << (hdr.isFake ? "假的" : "真的") << "\n";
        if (!hdr.content.empty()) {
            o << hdr.content;
            if (hdr.content.back() != '\n') o << "\n";
        }
        else {
            o << "   (空)\n";
        }
        o << "*/\n";
    }

    // 直接把解析结果作为源码输出
    o << text;
    if (!text.empty() && text.back() != '\n') o << "\n";

    return o.str();
}

// ---------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------
static void usage() {
    std::cout <<
        "kfc - F Language Compiler\n"
        "\n"
        "用法: kfc <input.kskbl> [options]\n"
        "\n"
        "选项:\n"
        "  -i <header>     指定头文件 (.zd.zdjd 或 .jd.zdjd)\n"
        "                  未指定时自动在源文件同目录下查找\n"
        "  -c, --compile   生成 C/C++ 代码后直接调用 gcc/g++ 编译\n"
        "  -o <path>       指定输出路径\n"
        "  -h, --help      显示此帮助\n";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    const std::string EXE_EXT = ".exe";
#else
    const std::string EXE_EXT = "";
#endif

    if (argc < 2) { usage(); return 1; }

    std::string input, output, headerPath;
    bool doCompile = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" || a == "--compile")      doCompile = true;
        else if (a == "-o" && i + 1 < argc)     output = argv[++i];
        else if (a == "-i" && i + 1 < argc)     headerPath = argv[++i];
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "未知选项: " << a << "\n"; return 1;
        }
        else input = a;
    }

    if (input.empty()) { usage(); return 1; }

    std::string src;
    if (!readFile(input, src)) {
        std::cerr << "[F] 无法打开源文件: " << input << "\n";
        return 1;
    }

    // ---------- 头文件：显式指定 或 自动查找 ----------
    HeaderResult hdr;
    bool hasHeader = false;

    if (!headerPath.empty()) {
        hdr = loadHeader(headerPath);
        if (!hdr.ok) {
            std::cerr << "\n[F] 头文件错误：\n\n"
                << hdr.error << "\n";
            return 1;
        }
        hasHeader = true;
    }
    else {
        std::string autoH = findAutoHeader(input);
        if (!autoH.empty()) {
            hdr = loadHeader(autoH);
            if (hdr.ok) hasHeader = true;
            // 自动找到但不合法就不吱声，交给用户显式 -i 时再报
        }
    }

    if (hasHeader) {
        std::cout << "[F] 头文件: " << hdr.path
            << (hdr.isFake ? "   [假的]" : "   [真的]")
            << "\n";
    }

    // ---------- 解析源文件 ----------
    ParseResult r = parse(src);
    if (!r.ok) {
        std::cerr << "\n[F] 编译失败：\n\n"
            << r.error << "\n";
        return 1;
    }

    const std::string ext = r.isCpp ? ".cpp" : ".c";

    std::string codeOut;
    if (!doCompile && !output.empty()) codeOut = output;
    else codeOut = dirName(input) + baseName(input) + ext;

    // ---------- 内部代码 ----------
    if (r.hitInternal) {
        std::string payload = internalCodePayload(r.internalCode);
        if (!writeFile(codeOut, payload)) {
            std::cerr << "[F] 无法写入产物文件: " << codeOut << "\n";
            return 1;
        }
        std::cout << "[F] 命中内部代码 " << r.internalCode
            << "（八进制 " << oct(r.internalCode) << "）\n"
            << "[F] 解析已停止，产物文件已覆盖 → " << codeOut << "\n";
        return 0;
    }

    // ---------- 正常生成 ----------
    std::string code = generateCode(r.text, r.isCpp, hdr, hasHeader);
    if (!writeFile(codeOut, code)) {
        std::cerr << "[F] 无法写入产物文件: " << codeOut << "\n";
        return 1;
    }
    std::cout << "[F] 已生成 " << (r.isCpp ? "C++" : "C") << " 代码 → "
        << codeOut << "  (" << r.text.size() << " 字节)\n";

    // ---------- 直接编译 ----------
    if (doCompile) {
        std::string exeOut = output.empty()
            ? dirName(input) + baseName(input) + EXE_EXT
            : output;
        std::string cc = r.isCpp ? "g++" : "gcc";
        std::string cmd = cc + " " + shq(codeOut) + " -o " + shq(exeOut);

        std::cout << "[F] 调用: " << cmd << "\n";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "[F] 外部编译器返回 " << rc << "，编译失败。\n";
            return 1;
        }
        std::cout << "[F] 编译成功 → " << exeOut << "\n";
    }

    return 0;
}
/**
 * =====================================================================
 *  《F语言规范条例》
 *  F Language Specification and Bylaws
 * ---------------------------------------------------------------------
 *  版本：1.0
 *  生效：自 .kskbl 第一次被成功解析之日起
 *  依据：F 语言设计哲学「梦到什么写什么」
 * ---------------------------------------------------------------------
 *  凡使用 .kskbl 源文件与 .zd.zdjd / .jd.zdjd 头文件的开发者，
 *  均视为已阅读、理解，并假装同意本规范的全部内容。
 * =====================================================================
 *
 *  ─────────────────────────────────────────────────────────────────
 *  序 言
 *  ─────────────────────────────────────────────────────────────────
 *
 *  F 语言的全称是 Forty-five，取自 '/' 与 '\' 在纸面上的书写角度
 *  约等于 45°。它只有两个符号，却试图完成从二进制到 ASCII 的
 *  全部旅程。它不追求实用，不追求性能，也不追求被任何人喜欢。
 *  它只追求一件事：梦到什么写什么。
 *
 *  因此，本规范与其说是在约束代码，不如说是在约束一种态度。
 *  以下内容，是使用 F 语言时必须遵守的全部规则。违反者不会
 *  被罚款，也不会被起诉，只会被编译器用仪式感的措辞请出去。
 *
 *  ─────────────────────────────────────────────────────────────────
 *  第一章  源文件与头文件
 *  ─────────────────────────────────────────────────────────────────
 *
 *  1.1  源文件
 *
 *  源文件后缀为 .kskbl，取自网络梗「康神开播了」。
 *  这个名字的意思是：程序要开始表演了。
 *  表演内容全部写在第一行，其余部分只是观众的掌声或嘘声。
 *
 *  1.2  头文件
 *
 *  头文件只有两种合法后缀：
 *
 *      .zd.zdjd   —— 真的·真的假的
 *      .jd.zdjd   —— 假的·真的假的
 *
 *  其余一切后缀，包括但不限于 .h、.hpp、.zdjd、.zd、.jd，
 *  以及完全没有后缀的情况，一律判定为假，不予受理。
 *
 *  判定为假时，编译器会明确告知：
 *
 *      「您的头文件是假的！」
 *
 *  这不是玩笑，这是正式的驳回。
 *  头文件若被判为假，整个编译流程立即停止，不进入后续阶段。
 *
 *  1.3  头文件的真与假
 *
 *  以 .zd.zdjd 结尾的头文件，编译器称为「真的」。
 *  以 .jd.zdjd 结尾的头文件，编译器称为「假的」。
 *
 *  但要注意：名字里写「假的」并不等于不能用。
 *  它只是在使用时会被编译器阴阳一句：
 *
 *      —— 真的假的？假的！
 *
 *  然后照样接纳。这就是「真的假的」这句话的精神：
 *  你说了，我信了，但我也记着你说的是真是假。
 *
 *  ─────────────────────────────────────────────────────────────────
 *  第二章  第一行
 *  ─────────────────────────────────────────────────────────────────
 *
 *  2.1  唯一有效行
 *
 *  F 语言只认第一行。
 *  第一行是唯一的现实，其余行全部视为注释。
 *  这叫「梦到什么写什么，醒了就忘」。
 *
 *  第一行之外的内容，无论写得多认真、多工整、多有文采，
 *  编译器都不会看第二眼。
 *
 *  2.2  首字符定语言
 *
 *  第一行第一个字符决定目标语言：
 *
 *      '/'  → C
 *      '\'  → C++
 *
 *  两个都不选、选了别的、或者第一行是空的，
 *  请先去把 ASCII 表抄一遍，再来谈语言选择。
 *
 *  2.3  非法入境
 *
 *  第一行出现 '/' 和 '\' 以外的任何字符（换行符除外），
 *  一律视为非法入境，就地遣返，不予申诉。
 *
 *  理由：F 语言只有两个笔画，多一笔都是对 45° 的背叛。
 *
 *  2.4  九进制生物
 *
 *  第一行除去首字符后，长度必须是 9 的倍数。
 *
 *  F 语言是九进制生物，读不满 9 个字符就会浑身难受。
 *  补不齐的，请自行面壁，直到背出 9 的前 100 个倍数。
 *
 *  ─────────────────────────────────────────────────────────────────
 *  第三章  编码规则
 *  ─────────────────────────────────────────────────────────────────
 *
 *  3.1  九字符一组
 *
 *  从第一行第二个字符开始，每 9 个字符为一组。
 *  每一组中：
 *
 *      '/'  代表 0
 *      '\'  代表 1
 *
 *  按顺序拼成一个 9 位二进制数。
 *
 *  3.2  八进制与 ASCII
 *
 *  每 9 位二进制数转换为一个 3 位八进制数，取值范围 000 ~ 777。
 *  转换完成后：
 *
 *      八进制 000 ~ 377   → 按 ASCII 输出对应字符
 *      八进制 400 ~ 777   → 内部代码
 *
 *  这条分界线不是随意的。
 *  ASCII 表只到 255，即八进制 377。
 *  超过 377 的部分，F 语言决定自己留用，不给 ASCII。
 *
 *  3.3  内部代码
 *
 *  读到八进制 400 及以上时，编译器立即停止解析。
 *  第一行后面所有内容全部丢弃，不再有任何解析行为。
 *
 *  随后，编译器用 internalCodePayload() 的返回值
 *  覆盖产物文件。
 *
 *  覆盖之后，概不负责，也概不解释。
 *
 *  触发未定义的内部代码，一律关 10 年禁闭。
 *  禁闭期间不得写注释、不得看斜杠、不得问「真的假的」。
 *  禁闭室内循环播放 Hello World，直到你梦见 400 的八进制。
 *
 *  ─────────────────────────────────────────────────────────────────
 *  第四章  注释区
 *  ─────────────────────────────────────────────────────────────────
 *
 *  4.1  静音原则
 *
 *  注释区域应保持安静，不应有 '/' 这样的大嗓门。
 *  在注释里夹带 '/'，等同于在图书馆里拉小提琴。
 *
 *  请把 '/' 交还给第一行，第一行才是它唯一的舞台。
 *
 *  4.2  越狱未遂
 *
 *  注释区的反斜杠属于越狱未遂。
 *  请立即上缴，并手抄 ASCII 表 0~255 共 256 遍。
 *
 *  抄不完的，按第三章 3.3 条处理，
 *  刑期从抄完那一刻开始计算。
 *
 *  ─────────────────────────────────────────────────────────────────
 *  第五章  编译器
 *  ─────────────────────────────────────────────────────────────────
 *
 *  5.1  名称
 *
 *  本编译器正式名称是 kfc，全称 Kernel of F Compiler。
 *  它与某炸鸡品牌的重名纯属巧合。
 *  如果你坚持要理解为「肯德基编译器」，
 *  本规范不予纠正，也不予追认，只记录在案。
 *
 *  5.2  用法
 *
 *      kfc <input.kskbl> [-i <header>] [-c] [-o <output>]
 *
 *      -i <header>     指定头文件（必须 .zd.zdjd 或 .jd.zdjd 结尾）
 *                      不指定时自动在源文件同目录下查找
 *      -c, --compile   生成 C/C++ 代码后直接调用 gcc/g++ 编译
 *      -o <path>       指定输出路径
 *      -h, --help      显示帮助
 *
 *  5.3  仪式感错误
 *
 *  所有错误信息必须以仪式感呈现。
 *  禁止使用 "Syntax error"、"Unexpected token" 等冷冰冰的措辞。
 *
 *  一条合格的 F 语言错误提示必须讲清楚三件事：
 *
 *      (1) 你错在哪；
 *      (2) 你违反了哪一条；
 *      (3) 你该受到什么惩罚。
 *
 *  三者缺一不可，否则编译器自己面壁。
 *
 *  ─────────────────────────────────────────────────────────────────
 *  第六章  美学与哲学
 *  ─────────────────────────────────────────────────────────────────
 *
 *  6.1  四十五度美学
 *
 *  F 全称 Forty-five，因 '/' 与 '\' 书写角度约为 45°。
 *  45° 是 F 语言的美学底线。
 *
 *  任何试图把斜杠写成 30° 或 60° 的行为，
 *  都将在编译期被判定为「角度不端正」，直接拒绝。
 *
 *  6.2  悬空的 Java OC
 *
 *  悬空的 Java OC 不觉得很酷吗？
 *
 *  觉得很酷的，请去第三章 3.3 条报到。
 *  不觉得很酷的，也请去第三章 3.3 条报到——
 *  因为禁闭室不看态度，只看行为。
 *
 *  ─────────────────────────────────────────────────────────────────
 *  附 则
 *  ─────────────────────────────────────────────────────────────────
 *
 *  本规范的最终解释权归「梦」所有。
 *
 *  梦到什么，就解释什么。
 *  如果解释不通，说明你还没睡醒。
 *  如果解释得通，说明你睡得太醒了，同样需要回去继续睡。
 *
 *  本规范自 .kskbl 第一次被成功解析之日起生效。
 *
 *  ---------------------------------------------------------------------
 *  真的假的？
 *  真的。
 * =====================================================================
 */