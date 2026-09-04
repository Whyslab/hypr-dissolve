// hypr-dissolve — распад окна на пиксели при закрытии, как в Telegram.
//
// Как это работает
// ----------------
// Когда окно закрывается, само окно уже мертво: Hyprland делает снимок
// (makeSnapshotFB), заворачивает его в CWindowFadeout и каждый кадр рисует
// через IHyprRenderer::renderFadeouts. Плагин перехватывает эту функцию и для
// оконных планов подменяет штатный CTexPassElement своим CDissolvePassElement.
//
// Планы слоёв и попапов (панели, меню, всплывашки) отдаются оригиналу без
// изменений — распадаться должны только окна.

#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/desktop/state/Fadeout.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>

#include "DissolvePass.hpp"

#include <algorithm>
#include <ranges>
#include <unordered_map>

inline HANDLE        PHANDLE = nullptr;
inline CFunctionHook* g_pFadeoutsHook = nullptr;

typedef void (*origRenderFadeouts)(void*, PHLMONITOR, Desktop::eFadeoutPlane, PHLWORKSPACE);

// Прогресс распада берём из штатной анимации fadeOut, но у полупрозрачных окон
// она стартует не с 1.0 — иначе распад начинался бы с середины. Поэтому для
// каждого фейдаута запоминаем альфу первого кадра и нормируем на неё.
static std::unordered_map<Desktop::IFadeout*, float> g_sourceAlpha;

static bool isWindowPlane(Desktop::eFadeoutPlane plane) {
    return plane == Desktop::FADEOUT_PLANE_WINDOW_TILED || plane == Desktop::FADEOUT_PLANE_WINDOW_FLOATING ||
        plane == Desktop::FADEOUT_PLANE_WINDOW_OVER_FULLSCREEN;
}

static void forgetDeadFadeouts() {
    if (g_sourceAlpha.empty())
        return;

    std::erase_if(g_sourceAlpha, [](const auto& entry) {
        const auto& [ptr, _] = entry;
        return std::ranges::none_of(Desktop::fadingOutState()->fadeouts(), [ptr](const auto& f) { return f.get() == ptr; });
    });
}

static void readConfig() {
    static auto PBLOCK    = CConfigValue<Hyprlang::FLOAT>("plugin:dissolve:block_size");
    static auto PRISE     = CConfigValue<Hyprlang::FLOAT>("plugin:dissolve:rise");
    static auto PSPREAD   = CConfigValue<Hyprlang::FLOAT>("plugin:dissolve:spread");
    static auto PDRIFT    = CConfigValue<Hyprlang::FLOAT>("plugin:dissolve:drift");
    static auto PLEAD     = CConfigValue<Hyprlang::FLOAT>("plugin:dissolve:lead");
    static auto PWAVE     = CConfigValue<Hyprlang::FLOAT>("plugin:dissolve:wave");
    static auto PLIFE     = CConfigValue<Hyprlang::FLOAT>("plugin:dissolve:dust_life");

    DissolveConfig::blockSize = std::max(1.F, (float)*PBLOCK);
    DissolveConfig::riseRange = std::max(0.F, (float)*PRISE);
    DissolveConfig::spread    = std::clamp((float)*PSPREAD, 0.F, 1.F);
    DissolveConfig::drift     = std::clamp((float)*PDRIFT, 0.F, 2.F);
    DissolveConfig::bodyLead  = std::max(1.F, (float)*PLEAD);
    DissolveConfig::wave      = std::clamp((float)*PWAVE, 0.F, 1.F);
    DissolveConfig::dustLife  = std::clamp((float)*PLIFE, 0.05F, 1.F);
}

static void hkRenderFadeouts(void* thisptr, PHLMONITOR monitor, Desktop::eFadeoutPlane plane, PHLWORKSPACE workspace) {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("plugin:dissolve:enabled");

    const auto  ORIG = (origRenderFadeouts)g_pFadeoutsHook->m_original;

    if (!monitor || !isWindowPlane(plane) || !*PENABLED) {
        ORIG(thisptr, monitor, plane, workspace);
        return;
    }

    forgetDeadFadeouts();
    readConfig();

    // Дальше — та же выборка и сортировка, что в оригинале (Renderer.cpp:3228),
    // иначе порядок перекрытия закрывающихся окон разъедется.
    std::vector<SP<Desktop::IFadeout>> fadeouts;
    for (auto const& fadeout : Desktop::fadingOutState()->fadeouts()) {
        if (!fadeout || fadeout->monitor() != monitor || fadeout->plane() != plane)
            continue;

        if (fadeout->workspace() && fadeout->workspace() != workspace)
            continue;

        fadeouts.emplace_back(fadeout);
    }

    if (fadeouts.empty())
        return;

    std::ranges::sort(fadeouts, {}, [](const auto& fadeout) { return fadeout->zIndex(); });

    const CRegion FAKEDAMAGE{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};

    for (auto const& fadeout : fadeouts) {
        const auto FB = fadeout->framebuffer();
        if (!FB || !FB->getTexture())
            continue;

        const float NOW    = fadeout->alpha();
        auto&       SOURCE = g_sourceAlpha.try_emplace(fadeout.get(), std::max(NOW, 0.01F)).first->second;
        SOURCE             = std::max(SOURCE, NOW); // на случай, если первый кадр застал анимацию не в начале

        const float PROGRESS = std::clamp(1.F - NOW / SOURCE, 0.F, 1.F);

        CBox        box = fadeout->renderBox();
        // Расширяем бокс вверх и в стороны: частицы должны улетать за границы
        // окна, иначе они упрутся в его край и эффект развалится.
        // Конфиг задаёт размеры в логических пикселях — приводим к физическим,
        // в которых живут renderBox и координаты внутри шейдера.
        const float SCALE   = monitor->m_scale;
        const float BLOCKPX = std::max(1.F, DissolveConfig::blockSize * SCALE);
        const float RISEPX  = DissolveConfig::riseRange * SCALE;

        // Запас должен покрывать САМУЮ быструю колонку, иначе её частицы упрутся
        // в край бокса и обрежутся ровной линией.
        const float PADY = RISEPX * (1.F + DissolveConfig::spread);
        // Горизонтальный запас — под боковой разброс блоков.
        const float PADX = RISEPX * DissolveConfig::drift + BLOCKPX * 2.F;

        CBox        grown = box.copy().expand(0.0);
        grown.x -= PADX;
        grown.y -= PADY;
        grown.w += PADX * 2.F;
        grown.h += PADY;

        // Область над окном сама по себе не попадёт в damage кадра — окна там
        // нет. Без этого частицы рисуются в уже отсечённую scissor'ом зону и
        // просто не видны.
        CBox logical = grown.copy().scale(1.F / monitor->m_scale);
        logical.x += monitor->m_position.x;
        logical.y += monitor->m_position.y;
        g_pHyprRenderer->damageBox(logical);

        CDissolvePassElement::SData data;
        data.tex      = FB->getTexture();
        data.box      = grown;
        data.pad      = {PADX, PADY};
        data.srcSize  = {box.w, box.h};
        data.blockPx  = BLOCKPX;
        data.risePx   = RISEPX;
        data.progress = PROGRESS;
        // Гасим не общей альфой, а поблочно в шейдере — иначе окно успевало бы
        // выцвести раньше, чем рассыплется.
        data.alpha  = 1.F;
        data.damage = FAKEDAMAGE;

        g_pHyprRenderer->addPassElement(makeUnique<CDissolvePassElement>(std::move(data)));
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Плагин лезет во внутренние структуры Hyprland, разложение которых меняется
    // между версиями. Несовпадение — не «может глючить», а почти гарантированный
    // segfault, а он уносит всю сессию с открытыми окнами. Поэтому отказываемся
    // грузиться, а не пытаемся работать.
    const std::string RUNNING = HyprlandAPI::getHyprlandVersion(PHANDLE).tag;
    if (RUNNING.find(DISSOLVE_BUILT_FOR) == std::string::npos) {
        HyprlandAPI::addNotification(PHANDLE,
                                     "[hypr-dissolve] Собран под Hyprland " DISSOLVE_BUILT_FOR ", а запущен " + RUNNING + ". Пересоберите плагин.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 10000);
        throw std::runtime_error("[hypr-dissolve] построен под другую версию Hyprland");
    }

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:block_size", Hyprlang::FLOAT{4.F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:rise", Hyprlang::FLOAT{200.F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:spread", Hyprlang::FLOAT{0.55F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:drift", Hyprlang::FLOAT{0.35F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:lead", Hyprlang::FLOAT{1.15F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:wave", Hyprlang::FLOAT{0.55F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:dust_life", Hyprlang::FLOAT{0.5F});

    auto matches = HyprlandAPI::findFunctionsByName(PHANDLE, "renderFadeouts");
    if (matches.empty()) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-dissolve] renderFadeouts не найдена — эффект не подключён.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hypr-dissolve] hook target missing");
    }

    g_pFadeoutsHook = HyprlandAPI::createFunctionHook(PHANDLE, matches[0].address, (void*)&hkRenderFadeouts);
    g_pFadeoutsHook->hook();

    HyprlandAPI::reloadConfig();

    return {"hypr-dissolve", "Распад окна на пиксели при закрытии", "bogdan", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_pFadeoutsHook)
        g_pFadeoutsHook->unhook();

    g_sourceAlpha.clear();
    dissolveDestroyShader();
}
