#include "DissolvePass.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>

#include <cmath>

using namespace Render;
using namespace Render::GL;

namespace DissolveConfig {
    float blockSize = 4.F;
    float riseRange = 200.F;
    float spread    = 0.55F;
    float drift     = 0.35F;
    float bodyLead  = 1.15F;
    float wave      = 0.55F;
    float dustLife  = 0.5F;
};

// Снимок окна режется на сетку блоков, и КАЖДЫЙ блок рисуется отдельным квадом
// через glDrawArraysInstanced — вся математика полёта живёт в вершинном шейдере.
//
// Почему не проще, одним квадом с фрагментным шейдером: там выборка обратная (для
// пикселя экрана ищем, откуда он взялся), а значит смещение частицы не может
// зависеть от строки — иначе чтобы найти строку-источник, нужно уже знать
// смещение. Подъём приходится делать общим для всей колонки, и столбец пикселей
// размазывается в сплошную вертикальную полосу вместо распада на частицы.
// Здесь отображение прямое, поэтому блок летит куда угодно и остаётся блоком.
//
// Вершин не передаём вообще: и угол квада, и номер блока считаются из
// gl_VertexID/gl_InstanceID, поэтому VAO нужен пустой.
//
// Имена uniform'ов продиктованы Hyprland (CShader::getUniformLocations ищет их
// по жёстко зашитому списку), свои не добавить — подходящие по типу заняты под
// свой смысл:
//   time        -> прогресс распада 0..1
//   fullSize    -> размер расширенного бокса, px
//   topLeft     -> отступ бокса относительно окна, px
//   bottomRight -> размер снимка окна, px
//   radius      -> сторона блока, px
//   range       -> дальность разлёта, px
//   noise       -> разброс скоростей блоков
//   contrast    -> боковой разброс
//   brightness  -> опережение распада
//   thick       -> волна: насколько верх осыпается раньше низа
//   shadowPower -> сколько блок живёт после отрыва
static const char* VERT = R"#(#version 300 es
precision highp float;

uniform mat3  proj;
uniform vec2  fullSize;
uniform vec2  topLeft;
uniform vec2  bottomRight;
uniform float radius;
uniform float time;
uniform float range;
uniform float noise;
uniform float contrast;
uniform float brightness;
uniform float thick;
uniform float shadowPower;

out vec2  v_uv;
out float v_alpha;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float blk  = max(radius, 1.0);
    vec2  win  = bottomRight;
    int   cols = int(ceil(win.x / blk));

    vec2  b        = vec2(float(gl_InstanceID % cols), float(gl_InstanceID / cols));
    vec2  blockPos = b * blk;

    // Порядок вершин повторяет штатный fullVerts: TL, BL, TR, BR.
    vec2  corner = vec2(float(gl_VertexID / 2), float(gl_VertexID % 2));
    vec2  local  = blockPos + corner * blk;

    v_uv = clamp(local / win, vec2(0.0), vec2(1.0));

    // Момент отрыва блока. Чистый хеш дал бы равномерный «телевизионный снег»
    // по всей площади; примешивая высоту, получаем фронт, идущий сверху вниз.
    float centerY = (blockPos.y + blk * 0.5) / max(win.y, 1.0);
    float thr     = mix(hash12(b), clamp(centerY, 0.0, 1.0), thick);
    float p       = clamp(time, 0.0, 1.0) * brightness;

    vec2  offset = vec2(0.0);
    float a      = 1.0;

    if (p > thr) {
        float life = clamp((p - thr) / max(shadowPower, 0.05), 0.0, 1.0);
        float sp   = mix(1.0 - noise, 1.0 + noise, hash12(b + 7.3));
        float side = (hash12(b + 3.1) - 0.5) * 2.0;

        // Разгон нелинейный: блок трогается плавно, потом уносится.
        float t = pow(life, 1.35);
        offset  = vec2(side * range * contrast * t, -range * sp * t);
        a       = 1.0 - life;
    }

    if (a <= 0.0) {
        // Догоревший блок убираем за пределы клипа — дешевле, чем discard.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_alpha     = 0.0;
        return;
    }

    vec2 posBox = topLeft + local + offset;
    gl_Position = vec4(proj * vec3(posBox / fullSize, 1.0), 1.0);
    v_alpha     = a;
}
)#";

static const char* FRAG = R"#(#version 300 es
precision highp float;

in vec2  v_uv;
in float v_alpha;

out vec4 fragColor;

uniform sampler2D tex;
uniform float     alpha;

void main() {
    if (v_alpha <= 0.002)
        discard;

    // Текстуры окон premultiplied, поэтому просто домножаем.
    fragColor = texture(tex, v_uv) * v_alpha * alpha;
}
)#";

static SP<CShader> g_shader;

void               dissolveDestroyShader() {
    if (!g_shader)
        return;

    g_shader->destroy();
    g_shader.reset();
}

static bool ensureShader() {
    if (g_shader && g_shader->program())
        return true;

    g_shader = makeShared<CShader>();
    if (!g_shader->createProgram(VERT, FRAG, true, false)) {
        g_shader.reset();
        return false;
    }

    return true;
}

CDissolvePassElement::CDissolvePassElement(SData&& data) : m_data(std::move(data)) {
    ;
}

std::optional<CBox> CDissolvePassElement::boundingBox() {
    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor;
    if (!MONITOR)
        return m_data.box;

    return m_data.box.copy().scale(1.F / MONITOR->m_scale).round();
}

std::vector<UP<IPassElement>> CDissolvePassElement::draw() {
    if (!g_pHyprOpenGL || !m_data.tex || !m_data.tex->ok())
        return {};

    if (!ensureShader())
        return {};

    const float BLK = std::max(1.F, m_data.blockPx);
    const int   COLS = (int)std::ceil(m_data.srcSize.x / BLK);
    const int   ROWS = (int)std::ceil(m_data.srcSize.y / BLK);
    if (COLS <= 0 || ROWS <= 0)
        return {};

    CBox box = m_data.box;
    g_pHyprRenderer->m_renderData.renderModif.applyToBox(box);

    const auto MATRIX = g_pHyprRenderer->projectBoxToTarget(box);

    glActiveTexture(GL_TEXTURE0);
    m_data.tex->bind();
    m_data.tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_data.tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_data.tex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_data.tex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    auto shader = g_pHyprOpenGL->useShader(g_shader);

    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, MATRIX.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);
    shader->setUniformFloat(SHADER_TIME, m_data.progress);
    shader->setUniformFloat(SHADER_ALPHA, m_data.alpha);
    shader->setUniformFloat2(SHADER_FULL_SIZE, box.w, box.h);
    shader->setUniformFloat2(SHADER_TOP_LEFT, m_data.pad.x, m_data.pad.y);
    shader->setUniformFloat2(SHADER_BOTTOM_RIGHT, m_data.srcSize.x, m_data.srcSize.y);
    // Размеры уже в физических пикселях: координаты внутри шейдера тоже
    // физические, иначе на мониторе со scale 2 эффект был бы вдвое слабее.
    shader->setUniformFloat(SHADER_RADIUS, BLK);
    shader->setUniformFloat(SHADER_RANGE, m_data.risePx);
    shader->setUniformFloat(SHADER_NOISE, DissolveConfig::spread);
    shader->setUniformFloat(SHADER_CONTRAST, DissolveConfig::drift);
    shader->setUniformFloat(SHADER_BRIGHTNESS, DissolveConfig::bodyLead);
    shader->setUniformFloat(SHADER_THICK, DissolveConfig::wave);
    shader->setUniformFloat(SHADER_SHADOW_POWER, DissolveConfig::dustLife);

    g_pHyprOpenGL->blend(true);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));

    const GLsizei INSTANCES = (GLsizei)(COLS * ROWS);

    m_data.damage.forEachRect([INSTANCES](const auto& RECT) {
        g_pHyprOpenGL->scissor(&RECT, g_pHyprRenderer->m_renderData.transformDamage);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, INSTANCES);
    });

    g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
    m_data.tex->unbind();

    return {};
}
