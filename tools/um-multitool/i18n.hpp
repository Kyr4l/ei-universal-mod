// The GUI's display language. English is the language of the source: every UI text in the code is
// English, and for Russian the texts are swapped at drawing time by the hook in vendor/imgui
// (UmTranslateText for plain text, UmTranslateFmt for printf-style formats) using the table in
// lang/ru.txt (beside the program: editable without a rebuild). A text without an entry in the table stays English. The choice is kept in
// um-multitool.cfg (LANGUAGE=en / ru); when it is missing the first-start popup asks for it.
#pragma once

#include <string>

namespace i18n {

enum class Lang { English = 0, Russian = 1 };

Lang Current();
void Set(Lang lang);

inline const char* Code(Lang lang) { return lang == Lang::Russian ? "ru" : "en"; }
inline const char* NativeName(Lang lang) { return lang == Lang::Russian ? "Русский" : "English"; }
// "en" / "ru" (anything else: false).
bool FromCode(const std::string& code, Lang& out);

// The translation of an English UI text now (the text itself when there is none or the language
// is English). For texts that are built or compared in code rather than drawn straight away.
const char* Tr(const char* english);

// While one lives, drawn texts are not translated: for data shown as it is (a script, a file's text)
// whose words may also be UI texts ("Script", "Unit", "Move"). Text boxes do this for what is typed in them.
struct Verbatim {
    Verbatim();
    ~Verbatim();
    Verbatim(const Verbatim&) = delete;
    Verbatim& operator=(const Verbatim&) = delete;
};

} // namespace i18n
