// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The texts of the launcher's dialogs. English is the language of the source; another language comes from EIStarter's lang\<code>\lang.ini
// files (UTF-16 with a BOM; sections [Options] and [ModInfo], the keys are the control names: lbTexturesQ, cbbAudio__0, btOk ...; <rn> in a
// text is a line break), so the translations made for EIStarter keep working. A text without an entry stays English, except the launcher's own newer
// texts (kRussian below), which have a Russian version built in.
#pragma once

#include <cstring>
#include <string>

#include "inifile.hpp"

namespace launcher {

// The launcher's own texts that EIStarter's lang.ini files do not have (the key is the control name used in the code).
struct BuiltinText { const char* key; const char* russian; };
inline const BuiltinText kRussian[] = {
    {"btCameraOriginal", "Камера из оригинала"},
    {"tipCameraOriginal", "Заменяет все настройки камеры значениями оригинального выпуска игры на CD.\nПрежние значения сохраняются в EIStarter-UM-camera-backup.txt."},
    {"camTitle", "Настройки камеры"},
    {"camQuestion", "Настройки камеры отличаются от оригинального выпуска Evil Islands на CD. Заменить их оригинальными? Прежние значения будут сохранены в EIStarter-UM-camera-backup.txt."},
    {"camCount", "Отличающихся значений: %d."},
    {"camColName", "Значение"},
    {"camColMine", "Ваше"},
    {"camColOriginal", "Оригинал"},
    {"camSame", "Как в оригинале."},
    {"camReplace", "Заменить"},
    {"camKeep", "Оставить"},
    {"camDontShow", "Больше не показывать"},
    {"resNotOffered", "экран не поддерживает"},
    {"tipResNotOffered", "У вашего экрана нет 32-битного режима такого размера: игра может не запуститься.\nВыберите режим из списка."},
    // Options > Renderer (0.3.0)
    {"tabRenderer", "Рендер"},
    {"lbRenderer", "Рендер"},
    {"cbbRenderer__0", "Авто"},
    {"cbbRenderer__2", "Родной"},
    {"tipRenderer", "Авто: D7VK, если он работает на этом ПК, иначе родной.\nD7VK: DirectDraw через Vulkan: быстрее, MSAA, анизотропия, солнечные тени Universal Mod.\nРодной: собственный DirectDraw игры."},
    {"d7vkWorks", "D7VK здесь работает."},
    {"d7vkUnchecked", "D7VK ещё не проверен."},
    {"d7vkFails", "D7VK здесь не работает:"},
    {"btD7vkCheck", "Проверить"},
    {"tipD7vkCheck", "Проверяет D7VK сейчас (несколько секунд)."},
    {"tipMsaa", "Мультисэмплинг до 4x: ровные края без размытия."},
    {"lbAnisotropy", "Анизотропия"},
    {"cbbAnisotropy__0", "Выкл."},
    {"tipAnisotropy", "Чётче земля и стены вдали."},
    {"cbbVsync__0", "По умолчанию"},
    {"cbbVsync__1", "Вкл."},
    {"cbbVsync__2", "Выкл."},
    {"tipVsync", "По умолчанию: решает Engine\\dxvk.conf."},
    {"lbFpsLimit", "Лимит FPS"},
    {"tipFpsLimit", "0 = без лимита."},
    {"lbLatency", "Задержка"},
    {"cbbLatency__0", "По умолчанию"},
    {"tipLatency", "Кадров, готовящихся заранее. 1 = наименьшая задержка ввода."},
    {"cbFpsOverlay", "Счётчик FPS"},
    {"tipFpsOverlay", "Счётчик кадров D7VK, слева сверху."},
    {"tipAntialiasingMsaa", "MSAA включён на вкладке «Рендер» (D7VK). Оставьте это выключенным."},
};

class Texts {
public:
    void Load(const std::string& launcherDir, const std::string& lang) {
        lang_ = lang;
        ini_ = IniFile();
        std::string path = launcherDir + "/lang/" + lang + "/lang.ini";
        loaded_ = ini_.Load(path);
    }
    // The text for `key` in `section`, or `english`.
    std::string Get(const char* section, const char* key, const char* english) const {
        std::string v = loaded_ ? ini_.Get(section, key, "\x01") : "\x01";
        if (v == "\x01") {
            v = english;
            if (lang_ == "ru") for (const BuiltinText& b : kRussian) if (std::strcmp(b.key, key) == 0) v = b.russian;
        }
        for (size_t p; (p = v.find("<rn>")) != std::string::npos;) v.replace(p, 4, "\n");
        return v;
    }
    bool Loaded() const { return loaded_; }

private:
    IniFile ini_;
    bool loaded_ = false;
    std::string lang_;
};

} // namespace launcher
