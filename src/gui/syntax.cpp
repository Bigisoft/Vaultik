#include "syntax.h"

#include <QFileInfo>
#include <QStringList>

#include "theme.h"

namespace {

enum class Kind { Code, Markup, Json, Yaml, Ini, Markdown, Css };

struct Language {
    const char* name;
    QStringList extensions;
    Kind kind = Kind::Code;
    QStringList keywords;
    QStringList types;
    QStringList line_comments;
    QList<QPair<QString, QString>> block_comments;
    QString quotes;          // characters that start and end strings
    bool case_insensitive = false;
    bool preprocessor = false;  // lines starting with #
    bool variables = false;     // $name
    bool capitalised_types = false;
};

QStringList words(const char* text) {
    return QString::fromLatin1(text).split(' ', Qt::SkipEmptyParts);
}

const std::vector<Language>& languages() {
    static const std::vector<Language> all = [] {
        std::vector<Language> v;
        const QStringList c_comments = {QStringLiteral("//")};
        const QList<QPair<QString, QString>> c_blocks = {{QStringLiteral("/*"), QStringLiteral("*/")}};

        Language cpp{"C++", words("c h cpp hpp cc hh cxx hxx inl ipp ino")};
        cpp.keywords = words(
            "alignas alignof asm auto break case catch class const consteval constexpr constinit const_cast continue "
            "co_await co_return co_yield decltype default delete do dynamic_cast else enum explicit export extern false "
            "final for friend goto if inline mutable namespace new noexcept nullptr operator override private protected "
            "public register reinterpret_cast requires return sizeof static static_assert static_cast struct switch "
            "template this thread_local throw true try typedef typeid typename union using virtual volatile while");
        cpp.types = words("bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t "
                          "size_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t std");
        cpp.line_comments = c_comments;
        cpp.block_comments = c_blocks;
        cpp.quotes = QStringLiteral("\"'");
        cpp.preprocessor = true;
        cpp.capitalised_types = true;
        v.push_back(cpp);

        Language cs{"C#", words("cs csx")};
        cs.keywords = words(
            "abstract as async await base break case catch checked class const continue default delegate do else enum "
            "event explicit extern false finally fixed for foreach get goto if implicit in init interface internal is "
            "lock namespace new null operator out override params partial private protected public readonly record ref "
            "return sealed set sizeof stackalloc static struct switch this throw true try typeof unchecked unsafe using "
            "var virtual volatile when where while yield nameof");
        cs.types = words("bool byte char decimal double dynamic float int long nint nuint object sbyte short string uint "
                         "ulong ushort void");
        cs.line_comments = c_comments;
        cs.block_comments = c_blocks;
        cs.quotes = QStringLiteral("\"'");
        cs.preprocessor = true;
        cs.capitalised_types = true;
        v.push_back(cs);

        Language java{"Java", words("java kt kts scala groovy gradle")};
        java.keywords = words(
            "abstract assert break case catch class const continue default do else enum extends final finally for goto "
            "if implements import instanceof interface native new package private protected public return static "
            "strictfp super switch synchronized this throw throws transient try volatile while true false null var "
            "record yield fun val object companion data sealed when override open internal");
        java.types = words("boolean byte char double float int long short void String Int Long Double Boolean Unit Any");
        java.line_comments = c_comments;
        java.block_comments = c_blocks;
        java.quotes = QStringLiteral("\"'");
        java.capitalised_types = true;
        v.push_back(java);

        Language js{"JavaScript", words("js mjs cjs jsx ts tsx mts cts")};
        js.keywords = words(
            "abstract as async await break case catch class const continue debugger declare default delete do else enum "
            "export extends finally for from function get if implements import in instanceof interface keyof let namespace "
            "new of private protected public readonly return set static super switch this throw try type typeof var void "
            "while with yield true false null undefined");
        js.types = words("any boolean never number object string symbol unknown bigint");
        js.line_comments = c_comments;
        js.block_comments = c_blocks;
        js.quotes = QStringLiteral("\"'`");
        js.capitalised_types = true;
        v.push_back(js);

        Language go{"Go", words("go")};
        go.keywords = words("break case chan const continue default defer else fallthrough for func go goto if import "
                            "interface map package range return select struct switch type var true false nil");
        go.types = words("bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string "
                         "uint uint8 uint16 uint32 uint64 uintptr any");
        go.line_comments = c_comments;
        go.block_comments = c_blocks;
        go.quotes = QStringLiteral("\"'`");
        v.push_back(go);

        Language rust{"Rust", words("rs")};
        rust.keywords = words("as async await break const continue crate dyn else enum extern false fn for if impl in let "
                              "loop match mod move mut pub ref return self Self static struct super trait true type "
                              "unsafe use where while");
        rust.types = words("bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize String Vec Option "
                           "Result Box");
        rust.line_comments = c_comments;
        rust.block_comments = c_blocks;
        rust.quotes = QStringLiteral("\"");
        rust.capitalised_types = true;
        v.push_back(rust);

        Language py{"Python", words("py pyw pyi")};
        py.keywords = words("False None True and as assert async await break class continue def del elif else except "
                            "finally for from global if import in is lambda nonlocal not or pass raise return try while "
                            "with yield self match case");
        py.types = words("int float str bytes bool list dict set tuple object");
        py.line_comments = {QStringLiteral("#")};
        py.block_comments = {{QStringLiteral("\"\"\""), QStringLiteral("\"\"\"")}, {QStringLiteral("'''"), QStringLiteral("'''")}};
        py.quotes = QStringLiteral("\"'");
        py.capitalised_types = true;
        v.push_back(py);

        Language ps{"PowerShell", words("ps1 psm1 psd1")};
        ps.keywords = words("begin break catch class continue data do dynamicparam else elseif end enum exit filter finally "
                            "for foreach from function if in param process return switch throw trap try until using while "
                            "true false null");
        ps.line_comments = {QStringLiteral("#")};
        ps.block_comments = {{QStringLiteral("<#"), QStringLiteral("#>")}};
        ps.quotes = QStringLiteral("\"'");
        ps.case_insensitive = true;
        ps.variables = true;
        v.push_back(ps);

        Language sh{"Shell", words("sh bash zsh ksh")};
        sh.keywords = words("if then else elif fi case esac for while until do done in function return local export "
                            "readonly declare echo exit set unset shift source true false");
        sh.line_comments = {QStringLiteral("#")};
        sh.quotes = QStringLiteral("\"'");
        sh.variables = true;
        v.push_back(sh);

        Language bat{"Batch", words("bat cmd")};
        bat.keywords = words("echo set if else goto call exit for in do not exist defined errorlevel setlocal endlocal "
                             "pause shift start cd pushd popd off on equ neq lss leq gtr geq");
        bat.line_comments = {QStringLiteral("rem "), QStringLiteral("::")};
        bat.quotes = QStringLiteral("\"");
        bat.case_insensitive = true;
        v.push_back(bat);

        Language sql{"SQL", words("sql")};
        sql.keywords = words(
            "add all alter and as asc begin between by case check commit constraint create cross database declare default "
            "delete desc distinct drop else end exec execute exists foreign from full go group having if in index inner "
            "insert into is join key left like limit merge not null offset on or order outer output primary procedure "
            "references return right rollback schema select set table then top transaction trigger union unique update "
            "use values view when where while with function returns");
        sql.types = words("int bigint smallint tinyint bit decimal numeric float real money date datetime datetime2 time "
                          "char varchar nchar nvarchar text ntext uniqueidentifier varbinary xml");
        sql.line_comments = {QStringLiteral("--")};
        sql.block_comments = c_blocks;
        sql.quotes = QStringLiteral("'");
        sql.case_insensitive = true;
        v.push_back(sql);

        Language lua{"Lua", words("lua")};
        lua.keywords = words("and break do else elseif end false for function goto if in local nil not or repeat return "
                             "then true until while");
        lua.line_comments = {QStringLiteral("--")};
        lua.block_comments = {{QStringLiteral("--[["), QStringLiteral("]]")}};
        lua.quotes = QStringLiteral("\"'");
        v.push_back(lua);

        Language markup{"XML", words("xml html htm xhtml xaml svg csproj vcxproj props targets resx config manifest plist "
                                     "ui qrc")};
        markup.kind = Kind::Markup;
        markup.block_comments = {{QStringLiteral("<!--"), QStringLiteral("-->")}};
        v.push_back(markup);

        Language json{"JSON", words("json jsonc json5 geojson webmanifest")};
        json.kind = Kind::Json;
        json.keywords = words("true false null");
        json.line_comments = c_comments;
        json.block_comments = c_blocks;
        json.quotes = QStringLiteral("\"");
        v.push_back(json);

        Language yaml{"YAML", words("yaml yml")};
        yaml.kind = Kind::Yaml;
        yaml.keywords = words("true false null yes no on off");
        yaml.line_comments = {QStringLiteral("#")};
        yaml.quotes = QStringLiteral("\"'");
        v.push_back(yaml);

        Language ini{"INI", words("ini cfg conf toml properties inf reg editorconfig gitconfig")};
        ini.kind = Kind::Ini;
        ini.keywords = words("true false");
        ini.line_comments = {QStringLiteral(";"), QStringLiteral("#")};
        ini.quotes = QStringLiteral("\"'");
        v.push_back(ini);

        Language css{"CSS", words("css scss sass less qss")};
        css.kind = Kind::Css;
        css.line_comments = c_comments;
        css.block_comments = c_blocks;
        css.quotes = QStringLiteral("\"'");
        v.push_back(css);

        Language md{"Markdown", words("md markdown")};
        md.kind = Kind::Markdown;
        v.push_back(md);
        return v;
    }();
    return all;
}

struct Palette {
    QColor keyword, type, string, number, comment, preprocessor, function, tag, attribute, key, variable, heading;
};

Palette palette() {
    if (theme().dark) {
        return {QColor(0x56, 0x9C, 0xD6), QColor(0x4E, 0xC9, 0xB0), QColor(0xCE, 0x91, 0x78), QColor(0xB5, 0xCE, 0xA8),
                QColor(0x6A, 0x99, 0x55), QColor(0xC5, 0x86, 0xC0), QColor(0xDC, 0xDC, 0xAA), QColor(0x56, 0x9C, 0xD6),
                QColor(0x9C, 0xDC, 0xFE), QColor(0x9C, 0xDC, 0xFE), QColor(0x9C, 0xDC, 0xFE), QColor(0x56, 0x9C, 0xD6)};
    }
    return {QColor(0x00, 0x00, 0xFF), QColor(0x26, 0x7F, 0x99), QColor(0xA3, 0x15, 0x15), QColor(0x09, 0x86, 0x58),
            QColor(0x00, 0x80, 0x00), QColor(0xAF, 0x00, 0xDB), QColor(0x79, 0x5E, 0x26), QColor(0x80, 0x00, 0x00),
            QColor(0xE5, 0x00, 0x00), QColor(0x04, 0x51, 0xA5), QColor(0x00, 0x10, 0x80), QColor(0x00, 0x00, 0xFF)};
}

QTextCharFormat fmt(const QColor& c, bool bold = false, bool italic = false) {
    QTextCharFormat f;
    f.setForeground(c);
    if (bold) f.setFontWeight(QFont::Bold);
    if (italic) f.setFontItalic(true);
    return f;
}

QString word_list(const QStringList& list) {
    QStringList escaped;
    for (const QString& w : list) escaped << QRegularExpression::escape(w);
    return QStringLiteral("\\b(?:") + escaped.join('|') + QStringLiteral(")\\b");
}

}  // namespace

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}

SyntaxHighlighter* SyntaxHighlighter::for_file(const QString& file_name, QTextDocument* document) {
    const QFileInfo info(file_name);
    QString ext = info.suffix().toLower();
    if (ext.isEmpty()) ext = info.fileName().toLower().remove('.');  // ".editorconfig", "Makefile"
    const Language* lang = nullptr;
    for (const Language& l : languages())
        if (l.extensions.contains(ext)) lang = &l;
    if (lang == nullptr) return nullptr;

    auto* h = new SyntaxHighlighter(document);  // owned by the document
    h->language_ = QString::fromLatin1(lang->name);
    const Palette p = palette();
    const auto opts = lang->case_insensitive ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption;
    const auto add = [&](const QString& pattern, const QTextCharFormat& format, int group = 0) {
        h->rules_.push_back({QRegularExpression(pattern, opts), format, group});
    };

    switch (lang->kind) {
        case Kind::Markup:
            add(QStringLiteral("<[?!]?/?[A-Za-z_][\\w:.-]*|/?>|\\?>"), fmt(p.tag));
            add(QStringLiteral("\\s([A-Za-z_][\\w:.-]*)\\s*="), fmt(p.attribute), 1);
            add(QStringLiteral("\"[^\"]*\"|'[^']*'"), fmt(p.string));
            add(QStringLiteral("&[#\\w]+;"), fmt(p.preprocessor));
            break;
        case Kind::Markdown:
            add(QStringLiteral("^#{1,6}\\s.*$"), fmt(p.heading, true));
            add(QStringLiteral("\\*\\*[^*]+\\*\\*|__[^_]+__"), fmt(p.keyword, true));
            add(QStringLiteral("(?<![*\\w])\\*[^*\\s][^*]*\\*(?!\\*)|(?<![_\\w])_[^_\\s][^_]*_(?!_)"), fmt(p.type, false, true));
            add(QStringLiteral("`[^`]+`"), fmt(p.string));
            add(QStringLiteral("\\[[^\\]]*\\]\\([^)]*\\)"), fmt(p.attribute));
            add(QStringLiteral("^\\s*(?:[-*+]|\\d+\\.)\\s"), fmt(p.preprocessor, true));
            add(QStringLiteral("^>.*$"), fmt(p.comment, false, true));
            h->blocks_.push_back({QRegularExpression(QStringLiteral("^```")), QRegularExpression(QStringLiteral("^```")), fmt(p.string)});
            break;
        case Kind::Json:
            add(QStringLiteral("-?\\b\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b"), fmt(p.number));
            add(word_list(lang->keywords), fmt(p.keyword));
            add(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\""), fmt(p.string));
            add(QStringLiteral("(\"(?:[^\"\\\\]|\\\\.)*\")\\s*:"), fmt(p.key), 1);
            break;
        case Kind::Yaml:
            add(QStringLiteral("-?\\b\\d+(?:\\.\\d+)?\\b"), fmt(p.number));
            add(word_list(lang->keywords), fmt(p.keyword));
            add(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"|'[^']*'"), fmt(p.string));
            add(QStringLiteral("^\\s*(?:-\\s+)?([\\w.\\-/ ]+?)\\s*:(?:\\s|$)"), fmt(p.key), 1);
            add(QStringLiteral("^---.*$|^\\.\\.\\..*$"), fmt(p.preprocessor));
            add(QStringLiteral("(?:^|\\s)#.*$"), fmt(p.comment, false, true));
            break;
        case Kind::Ini:
            add(QStringLiteral("-?\\b\\d+(?:\\.\\d+)?\\b"), fmt(p.number));
            add(word_list(lang->keywords), fmt(p.keyword));
            add(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"|'[^']*'"), fmt(p.string));
            add(QStringLiteral("^\\s*([^=:#;\\[]+?)\\s*[=:]"), fmt(p.key), 1);
            add(QStringLiteral("^\\s*\\[[^\\]]*\\]"), fmt(p.type, true));
            add(QStringLiteral("^\\s*[;#].*$"), fmt(p.comment, false, true));
            break;
        case Kind::Css:
            add(QStringLiteral("#[0-9A-Fa-f]{3,8}\\b|-?\\b\\d+(?:\\.\\d+)?(?:px|em|rem|pt|%|vh|vw|s|ms|deg)?\\b"), fmt(p.number));
            add(QStringLiteral("([\\w-]+)\\s*:(?!:)"), fmt(p.attribute), 1);
            add(QStringLiteral("[.#][A-Za-z_][\\w-]*"), fmt(p.tag));
            add(QStringLiteral("@[\\w-]+"), fmt(p.keyword));
            add(QStringLiteral("\"[^\"]*\"|'[^']*'"), fmt(p.string));
            h->blocks_.push_back({QRegularExpression(QStringLiteral("/\\*")), QRegularExpression(QStringLiteral("\\*/")),
                                  fmt(p.comment, false, true)});
            add(QStringLiteral("//.*$"), fmt(p.comment, false, true));
            break;
        case Kind::Code: {
            add(QStringLiteral("\\b(?:0[xX][0-9A-Fa-f']+|0[bB][01']+|\\d[\\d']*(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)[uUlLfFdDmM]*\\b"),
                fmt(p.number));
            if (lang->capitalised_types) add(QStringLiteral("\\b[A-Z][a-z0-9]\\w*\\b"), fmt(p.type));
            add(QStringLiteral("\\b([A-Za-z_]\\w*)(?=\\s*\\()"), fmt(p.function), 1);
            if (!lang->types.isEmpty()) add(word_list(lang->types), fmt(p.type));
            if (!lang->keywords.isEmpty()) add(word_list(lang->keywords), fmt(p.keyword));
            if (lang->variables) add(QStringLiteral("\\$\\{?[A-Za-z_][\\w:]*\\}?|\\$\\d"), fmt(p.variable));
            if (lang->case_insensitive && QString::fromLatin1(lang->name) == QStringLiteral("PowerShell"))
                add(QStringLiteral("\\s(-[A-Za-z]+)\\b"), fmt(p.keyword), 1);
            if (lang->preprocessor) add(QStringLiteral("^\\s*#\\s*\\w+.*$"), fmt(p.preprocessor));
            // Strings, then comments, win over everything before them.
            for (const QChar q : lang->quotes) {
                const QString e = QRegularExpression::escape(QString(q));
                add(e + QStringLiteral("(?:[^") + e + QStringLiteral("\\\\]|\\\\.)*") + e, fmt(p.string));
            }
            for (const QString& c : lang->line_comments) {
                if (c.endsWith(' ')) add(QStringLiteral("^\\s*@?") + QRegularExpression::escape(c.trimmed()) + QStringLiteral("\\b.*$"), fmt(p.comment, false, true));
                else add(QRegularExpression::escape(c) + QStringLiteral(".*$"), fmt(p.comment, false, true));
            }
            break;
        }
    }
    if (lang->kind != Kind::Css && lang->kind != Kind::Markdown) {
        for (const auto& [start, end] : lang->block_comments)
            h->blocks_.push_back({QRegularExpression(QRegularExpression::escape(start)),
                                  QRegularExpression(QRegularExpression::escape(end)), fmt(p.comment, false, true)});
    }
    return h;
}

void SyntaxHighlighter::highlightBlock(const QString& text) {
    for (const Rule& rule : rules_) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (m.capturedLength(rule.group) > 0) setFormat(m.capturedStart(rule.group), m.capturedLength(rule.group), rule.format);
        }
    }

    // Comments and strings spanning lines. A block that starts on this line or continues from the
    // previous one colours everything up to its end.
    setCurrentBlockState(0);
    int pos = 0;
    int inside = previousBlockState();
    while (pos <= text.size()) {
        if (inside > 0 && inside <= static_cast<int>(blocks_.size())) {
            const Block& b = blocks_[static_cast<std::size_t>(inside - 1)];
            const QRegularExpressionMatch end = b.end.match(text, pos);
            if (!end.hasMatch()) {
                setFormat(pos, static_cast<int>(text.size()) - pos, b.format);
                setCurrentBlockState(inside);
                return;
            }
            const int stop = static_cast<int>(end.capturedEnd());
            setFormat(pos, stop - pos, b.format);
            pos = stop;
            inside = 0;
            continue;
        }
        // The earliest block start from here that is not inside a string or line comment.
        int best = -1;
        int best_at = static_cast<int>(text.size()) + 1;
        int best_len = 0;
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            const QRegularExpressionMatch m = blocks_[i].start.match(text, pos);
            if (m.hasMatch() && m.capturedStart() < best_at) {
                const QTextCharFormat here = format(static_cast<int>(m.capturedStart()));
                // Skip a start that the rules above put inside a string or a comment.
                if (here.foreground().color() == palette().string || here.fontItalic()) continue;
                best = static_cast<int>(i);
                best_at = static_cast<int>(m.capturedStart());
                best_len = static_cast<int>(m.capturedLength());
            }
        }
        if (best < 0) return;
        setFormat(best_at, best_len, blocks_[static_cast<std::size_t>(best)].format);
        pos = best_at + best_len;
        inside = best + 1;
    }
}
