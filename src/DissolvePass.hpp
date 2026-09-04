#pragma once

// Элемент рендера, который рисует снимок закрывающегося окна собственным
// шейдером «распада на пиксели» вместо штатного CTexPassElement.
//
// Регистрируется как EK_CUSTOM: Hyprland для таких элементов вызывает draw()
// (см. IElementRenderer::drawCustom), что и даёт возможность выполнить свои
// GL-вызовы внутри общего прохода рендера.

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprutils/math/Region.hpp>

class CDissolvePassElement : public IPassElement {
  public:
    struct SData {
        SP<Render::ITexture> tex;
        // Бокс, в который рисуем, — БОЛЬШЕ окна: частицы улетают за его границы.
        CBox     box;
        // На сколько бокс расширен относительно окна (слева/сверху), в пикселях.
        Vector2D pad;
        // Размер исходного снимка окна в пикселях.
        Vector2D srcSize;
        // 0 — окно целое, 1 — рассыпалось полностью.
        // Размер блока и дальность разлёта — в ФИЗИЧЕСКИХ пикселях (конфиг задаёт
        // их в логических, пересчёт на scale монитора делает main.cpp).
        float    blockPx  = 4.F;
        float    risePx   = 200.F;
        float    progress = 0.F;
        float    alpha    = 1.F;
        CRegion  damage;
    };

    explicit CDissolvePassElement(SData&& data);
    virtual ~CDissolvePassElement() = default;

    virtual std::vector<UP<IPassElement>> draw();

    virtual bool                         needsLiveBlur() {
        return false;
    }
    virtual bool needsPrecomputeBlur() {
        return false;
    }
    virtual const char* passName() {
        return "CDissolvePassElement";
    }
    virtual ePassElementType type() {
        return EK_CUSTOM;
    }
    virtual std::optional<CBox> boundingBox();
    virtual CRegion             opaqueRegion() {
        return {};
    }
    // Частицы полупрозрачные и разлетаются за пределы окна — упрощать проход
    // нельзя, иначе Hyprland решит, что под элементом ничего рисовать не надо.
    virtual bool disableSimplification() {
        return true;
    }

    SData m_data;
};

// Настройки читаются из конфига один раз за кадр в main.cpp.
namespace DissolveConfig {
    extern float blockSize; // размер «пикселя» распада, px
    extern float riseRange; // на сколько частицы улетают, px
    extern float spread;    // разброс скоростей колонок, 0..1
    extern float drift;     // боковой разброс блоков, доля от дальности
    extern float bodyLead;  // насколько распад тела опережает прогресс
    extern float wave;      // насколько верх окна осыпается раньше низа, 0..1
    extern float dustLife;  // сколько частица живёт после распада блока, 0..1
};

void dissolveDestroyShader();
