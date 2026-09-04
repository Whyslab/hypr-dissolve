// hypr-dissolve — распад окна на пиксели при закрытии, как в Telegram.
//
// Как это работает
// ----------------
// Когда окно закрывается, само окно уже мертво: Hyprland делает снимок
// (makeSnapshotFB), заворачивает его в CWindowFadeout и каждый кадр рисует
// через IHyprRenderer::renderFadeouts. Плагин перехватывает эту функцию и для
// оконных планов подменяет штатный CTexPassElement своим CDissolvePassElement.
//
// То же самое работает и со слоями (rofi и прочие меню на layer-shell), но
// только для тех, чьё имя перечислено в plugin:dissolve:layer_namespaces.
// Панель, уведомления и обои — тоже слои, и рассыпать их по умолчанию не надо.
// Попапы всегда идут мимо.
//
// Отдельно от анимации плагин умеет чинить утечку зажатой клавиши при смене
// фокуса — см. комментарий у hkSendEnter. По умолчанию выключено.

#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/desktop/state/Fadeout.hpp>
#include <hyprland/src/desktop/state/LayerFadeout.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/protocols/core/Seat.hpp>

#include <wayland-server-core.h>

#include "DissolvePass.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

inline HANDLE         PHANDLE            = nullptr;
inline CFunctionHook* g_pFadeoutsHook    = nullptr;
inline CFunctionHook* g_pLayerCreateHook = nullptr;
inline CFunctionHook* g_pSendEnterHook   = nullptr;
inline CFunctionHook* g_pSendKeyHook     = nullptr;

typedef void (*origRenderFadeouts)(void*, PHLMONITOR, Desktop::eFadeoutPlane, PHLWORKSPACE);
typedef SP<Desktop::CLayerFadeout> (*origLayerFadeoutCreate)(PHLLS, SP<Render::IFramebuffer>, float);
typedef void (*origSendEnter)(void*, SP<CWLSurfaceResource>, wl_array*);
typedef void (*origSendKey)(void*, uint32_t, uint32_t, wl_keyboard_key_state);

// Прогресс распада берём из штатной анимации fadeOut, но у полупрозрачных окон
// она стартует не с 1.0 — иначе распад начинался бы с середины. Поэтому для
// каждого фейдаута запоминаем альфу первого кадра и нормируем на неё.
static std::unordered_map<Desktop::IFadeout*, float> g_sourceAlpha;

// Имя слоя, по которому решается, рассыпать его или нет. Держим отдельной
// картой, потому что сам CLayerFadeout наружу ни слой, ни его имя не отдаёт:
// в классе только monitor/plane/zIndex/renderBox/alpha/done/effects. Ловим имя
// в момент создания фейдаута — там слой ещё под рукой.
static std::unordered_map<Desktop::IFadeout*, std::string> g_layerNs;

static bool isWindowPlane(Desktop::eFadeoutPlane plane) {
    return plane == Desktop::FADEOUT_PLANE_WINDOW_TILED || plane == Desktop::FADEOUT_PLANE_WINDOW_FLOATING ||
        plane == Desktop::FADEOUT_PLANE_WINDOW_OVER_FULLSCREEN;
}

static bool isLayerPlane(Desktop::eFadeoutPlane plane) {
    return plane == Desktop::FADEOUT_PLANE_LAYER_BACKGROUND || plane == Desktop::FADEOUT_PLANE_LAYER_BOTTOM || plane == Desktop::FADEOUT_PLANE_LAYER_TOP ||
        plane == Desktop::FADEOUT_PLANE_LAYER_OVERLAY;
}

static void forgetDeadFadeouts() {
    if (g_sourceAlpha.empty() && g_layerNs.empty())
        return;

    const auto ALIVE = [](Desktop::IFadeout* ptr) {
        return std::ranges::any_of(Desktop::fadingOutState()->fadeouts(), [ptr](const auto& f) { return f.get() == ptr; });
    };

    std::erase_if(g_sourceAlpha, [&](const auto& entry) { return !ALIVE(entry.first); });
    std::erase_if(g_layerNs, [&](const auto& entry) { return !ALIVE(entry.first); });
}

// Список разрешённых имён слоёв. Сравнение по подстроке без учёта регистра:
// std::regex в рендер-цикле — это и лишние аллокации на каждый кадр, и
// исключение на кривом вводе в конфиге.
static std::vector<std::string> g_nsAllow;
static std::string              g_nsRaw = "\x01"; // заведомо не равно любому конфигу

static std::string              lowered(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static void updateNamespaceList(const std::string& RAW) {
    if (RAW == g_nsRaw)
        return;

    g_nsRaw = RAW;
    g_nsAllow.clear();

    size_t start = 0;
    while (start <= RAW.size()) {
        const size_t COMMA = RAW.find(',', start);
        std::string  part  = RAW.substr(start, COMMA == std::string::npos ? std::string::npos : COMMA - start);

        const size_t FIRST = part.find_first_not_of(" \t");
        const size_t LAST  = part.find_last_not_of(" \t");
        if (FIRST != std::string::npos)
            g_nsAllow.emplace_back(lowered(part.substr(FIRST, LAST - FIRST + 1)));

        if (COMMA == std::string::npos)
            break;
        start = COMMA + 1;
    }
}

static bool namespaceAllowed(const std::string& ns) {
    const std::string NS = lowered(ns);
    return std::ranges::any_of(g_nsAllow, [&](const auto& entry) { return NS.find(entry) != std::string::npos; });
}

static bool shouldDissolve(Desktop::eFadeoutPlane plane, Desktop::IFadeout* fadeout) {
    static auto PLAYERS = CConfigValue<Hyprlang::INT>("plugin:dissolve:layers");

    if (isWindowPlane(plane))
        return true;

    if (!isLayerPlane(plane) || !*PLAYERS || !g_pLayerCreateHook)
        return false;

    const auto IT = g_layerNs.find(fadeout);
    if (IT == g_layerNs.end())
        return false; // имя не поймали — трогать не будем

    return namespaceAllowed(IT->second);
}

static SP<Desktop::CLayerFadeout> hkLayerFadeoutCreate(PHLLS layer, SP<Render::IFramebuffer> snapshot, float sourceAlpha) {
    const auto ORIG = (origLayerFadeoutCreate)g_pLayerCreateHook->m_original;
    auto       res  = ORIG(layer, snapshot, sourceAlpha);

    if (res && layer)
        g_layerNs[res.get()] = layer->m_namespace;


    return res;
}

// ---------------------------------------------------------------------------
// Утечка зажатой клавиши при смене фокуса
// ---------------------------------------------------------------------------
//
// По протоколу wl_keyboard.enter несёт массив клавиш, зажатых НА МОМЕНТ
// получения фокуса. Это состояние, а не события, но не все клиенты так его
// читают: Firefox видит в массиве Escape и обрабатывает как нажатие — то есть
// выходит из полноэкранного режима.
//
// Вылезает это каждый раз, когда окно или меню закрывается по нажатию Esc:
// фокус уходит вниз раньше, чем клавишу отпустили, и Esc достаётся тому, кто
// оказался под ним. Своё приложение можно научить закрываться на отпускании,
// но rofi и прочие чужие бинарники так не поправить — поэтому чистим массив
// на стороне композитора.
//
// Чистим ТОЛЬКО перечисленные коды, а не весь массив: играм и редакторам
// список зажатых клавиш нужен по делу (зажатая клавиша движения при возврате
// фокуса). Модификаторы идут отдельным путём, через sendMods, и фильтр их не
// касается вовсе.
static std::vector<uint32_t> g_keyLeakCodes;
static std::string           g_keyLeakRaw = "\x01";

static void                  updateKeyLeakCodes(const std::string& RAW) {
    if (RAW == g_keyLeakRaw)
        return;

    g_keyLeakRaw = RAW;
    g_keyLeakCodes.clear();

    size_t start = 0;
    while (start <= RAW.size()) {
        const size_t COMMA = RAW.find(',', start);
        std::string  part  = RAW.substr(start, COMMA == std::string::npos ? std::string::npos : COMMA - start);

        try {
            const int CODE = std::stoi(part);
            if (CODE >= 0)
                g_keyLeakCodes.emplace_back((uint32_t)CODE);
        } catch (...) {
            // мусор в конфиге — молча пропускаем, ронять композитор из-за
            // опечатки в списке кодов было бы обидно
        }

        if (COMMA == std::string::npos)
            break;
        start = COMMA + 1;
    }
}

// Что мы вырезали из enter у конкретного клиента. Без этого починка была бы
// половинчатой: клиенту сказано «ничего не зажато», но следом ему приезжает
// отпускание той самой клавиши. Firefox складывает одно с другим и считает, что
// нажатие состоялось — фуллскрин слетает ровно так же, как и без фильтра, хотя
// keydown в страницу не приходит вовсе.
static std::unordered_map<void*, std::unordered_set<uint32_t>> g_swallowed;

static void hkSendEnter(void* thisptr, SP<CWLSurfaceResource> surface, wl_array* keys) {
    static auto PFIX   = CConfigValue<Hyprlang::INT>("plugin:dissolve:key_leak_fix");
    static auto PCODES = CConfigValue<std::string>("plugin:dissolve:key_leak_codes");

    const auto  ORIG = (origSendEnter)g_pSendEnterHook->m_original;

    if (!*PFIX || !keys || keys->size == 0) {
        ORIG(thisptr, surface, keys);
        return;
    }

    updateKeyLeakCodes(*PCODES);

    if (g_keyLeakCodes.empty()) {
        ORIG(thisptr, surface, keys);
        return;
    }

    // Оригинальный массив принадлежит вызывающему коду и переиспользуется для
    // остальных клиентов — правим не его, а свою копию.
    wl_array filtered;
    wl_array_init(&filtered);

    const uint32_t* DATA  = (const uint32_t*)keys->data;
    const size_t    COUNT = keys->size / sizeof(uint32_t);
    bool            dropped = false;

    for (size_t i = 0; i < COUNT; ++i) {
        if (std::ranges::find(g_keyLeakCodes, DATA[i]) != g_keyLeakCodes.end()) {
            dropped = true;
            g_swallowed[thisptr].insert(DATA[i]);
            continue;
        }

        auto* slot = (uint32_t*)wl_array_add(&filtered, sizeof(uint32_t));
        if (!slot) { // не осталось памяти — отдаём как есть, это честнее пустого массива
            wl_array_release(&filtered);
            ORIG(thisptr, surface, keys);
            return;
        }

        *slot = DATA[i];
    }

    ORIG(thisptr, surface, dropped ? &filtered : keys);
    wl_array_release(&filtered);
}

static void hkSendKey(void* thisptr, uint32_t timeMs, uint32_t key, wl_keyboard_key_state state) {
    const auto ORIG = (origSendKey)g_pSendKeyHook->m_original;
    const auto IT   = g_swallowed.find(thisptr);

    if (IT == g_swallowed.end() || IT->second.empty()) {
        ORIG(thisptr, timeMs, key, state);
        return;
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // Настоящее нажатие: клиент теперь держит клавишу по-честному, и её
        // отпускание он должен получить.
        IT->second.erase(key);
        ORIG(thisptr, timeMs, key, state);
        return;
    }

    if (IT->second.erase(key) > 0)
        return; // отпускание без нажатия — глотаем, мы же скрыли и нажатие

    ORIG(thisptr, timeMs, key, state);
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
    // Верхняя граница 0.95, а не 1.0. Диапазон порогов в шейдере — 1 - dustLife,
    // и при dustLife = 1 он схлопнулся бы в ноль: последний блок начал бы гаснуть
    // в момент, когда шкала уже кончилась, и остался бы виден на 5%.
    DissolveConfig::dustLife  = std::clamp((float)*PLIFE, 0.05F, 0.95F);

    // Именно std::string, а не Hyprlang::STRING. У CConfigValue специализация
    // под строки написана только для std::string: она умеет достать значение из
    // m_hlangp, где и лежат строки, заведённые плагином. Общий шаблон читает
    // m_p, который в этом случае нулевой, — и разыменование роняет композитор
    // на первом же кадре.
    static auto PNS = CConfigValue<std::string>("plugin:dissolve:layer_namespaces");
    updateNamespaceList(*PNS);
}

static void hkRenderFadeouts(void* thisptr, PHLMONITOR monitor, Desktop::eFadeoutPlane plane, PHLWORKSPACE workspace) {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("plugin:dissolve:enabled");

    const auto  ORIG = (origRenderFadeouts)g_pFadeoutsHook->m_original;

    if (!monitor || plane == Desktop::FADEOUT_PLANE_POPUP || !*PENABLED) {
        ORIG(thisptr, monitor, plane, workspace);
        return;
    }

    forgetDeadFadeouts();
    readConfig();

    // Дальше — та же выборка и сортировка, что в оригинале (Renderer.cpp:3228),
    // иначе порядок перекрытия закрывающихся окон разъедется. Отличие одно:
    // делим фейдауты на «наши» и «чужие». Чужие бывают только среди слоёв —
    // панель или уведомление, которые гаснут в тот же момент, что и rofi.
    std::vector<SP<Desktop::IFadeout>> fadeouts;
    bool                               foreign = false;
    for (auto const& fadeout : Desktop::fadingOutState()->fadeouts()) {
        if (!fadeout || fadeout->monitor() != monitor || fadeout->plane() != plane)
            continue;

        if (fadeout->workspace() && fadeout->workspace() != workspace)
            continue;

        if (shouldDissolve(plane, fadeout.get()))
            fadeouts.emplace_back(fadeout);
        else
            foreign = true;
    }

    if (fadeouts.empty()) {
        ORIG(thisptr, monitor, plane, workspace);
        return;
    }

    if (foreign) {
        // Оригинал рисует ВСЕ фейдауты своей плоскости, поэтому просто позвать
        // его нельзя: наши он нарисует вторым слоем, и рядом с распадом будет
        // висеть целая копия окна. Прячем свои на время его вызова — так чужие
        // слои рисуются штатно, со всеми эффектами, а не нашей самоделкой.
        auto& LIST = const_cast<std::vector<SP<Desktop::IFadeout>>&>(Desktop::fadingOutState()->fadeouts());

        std::vector<SP<Desktop::IFadeout>> stashed;
        std::erase_if(LIST, [&](const SP<Desktop::IFadeout>& f) {
            if (std::ranges::find(fadeouts, f) == fadeouts.end())
                return false;

            stashed.emplace_back(f); // держим SP, иначе объект умрёт при удалении из списка
            return true;
        });

        ORIG(thisptr, monitor, plane, workspace);

        LIST.insert(LIST.end(), stashed.begin(), stashed.end());
    }

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
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:dust_life", Hyprlang::FLOAT{0.35F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:layers", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:layer_namespaces", Hyprlang::STRING{"rofi"});
    // Выключено по умолчанию: хук зовётся на каждой смене фокуса, это не
    // рендер, и включать его стоит осознанно. 1 — это KEY_ESC из evdev.
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:key_leak_fix", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:dissolve:key_leak_codes", Hyprlang::STRING{"1"});

    auto matches = HyprlandAPI::findFunctionsByName(PHANDLE, "renderFadeouts");
    if (matches.empty()) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-dissolve] renderFadeouts не найдена — эффект не подключён.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hypr-dissolve] hook target missing");
    }

    g_pFadeoutsHook = HyprlandAPI::createFunctionHook(PHANDLE, matches[0].address, (void*)&hkRenderFadeouts);
    g_pFadeoutsHook->hook();

    // Второй хук — только ради имени слоя. Без него распад слоёв работать может,
    // но не сможет отличить rofi от панели, поэтому при неудаче не падаем, а
    // глушим слои: пусть лучше не будет эффекта, чем рассыплется панель.
    for (auto const& match : HyprlandAPI::findFunctionsByName(PHANDLE, "create")) {
        // Сравниваем ИМЕННО mangled-сигнатуру. Поле demangled у
        // findFunctionsByName не соответствует своей же signature: в выдаче
        // 0.56.2 попадается запись, где demangled — деструктор CLayerFadeout,
        // а signature — create у CPopupFadeout. Отбор по demangled поэтому
        // молча промахивается, и распад слоёв просто не включается.
        if (!match.signature.contains("_ZN7Desktop13CLayerFadeout6createE"))
            continue;

        g_pLayerCreateHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, (void*)&hkLayerFadeoutCreate);
        g_pLayerCreateHook->hook();
        break;
    }


    if (!g_pLayerCreateHook)
        HyprlandAPI::addNotification(PHANDLE, "[hypr-dissolve] CLayerFadeout::create не найдена — распад слоёв выключен.", CHyprColor{1.0, 0.7, 0.2, 1.0}, 8000);

    for (auto const& match : HyprlandAPI::findFunctionsByName(PHANDLE, "sendEnter")) {
        // Опять по mangled: sendEnter есть у указателя, у data device и у
        // протокольной обёртки CWlKeyboard. Нужна именно CWLKeyboardResource.
        if (!match.signature.contains("_ZN19CWLKeyboardResource9sendEnterE"))
            continue;

        g_pSendEnterHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, (void*)&hkSendEnter);
        g_pSendEnterHook->hook();
        break;
    }


    for (auto const& match : HyprlandAPI::findFunctionsByName(PHANDLE, "sendKey")) {
        if (!match.signature.contains("_ZN19CWLKeyboardResource7sendKeyE"))
            continue;

        g_pSendKeyHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, (void*)&hkSendKey);
        g_pSendKeyHook->hook();
        break;
    }

    if (!g_pSendEnterHook || !g_pSendKeyHook)
        HyprlandAPI::addNotification(PHANDLE, "[hypr-dissolve] хуки клавиатуры не найдены — key_leak_fix работать не будет.", CHyprColor{1.0, 0.7, 0.2, 1.0}, 8000);

    HyprlandAPI::reloadConfig();

    return {"hypr-dissolve", "Распад окна на пиксели при закрытии", "bogdan", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_pFadeoutsHook)
        g_pFadeoutsHook->unhook();

    if (g_pLayerCreateHook)
        g_pLayerCreateHook->unhook();

    if (g_pSendEnterHook)
        g_pSendEnterHook->unhook();

    if (g_pSendKeyHook)
        g_pSendKeyHook->unhook();

    g_sourceAlpha.clear();
    g_layerNs.clear();
    g_swallowed.clear();
    dissolveDestroyShader();
}
