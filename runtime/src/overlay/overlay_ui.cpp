// The overlay's look (see overlay_ui.h). Style sizes follow Savage Wind's menu (runtime/ui/menu.mm style_init, MIT);
// the colours are SuperWonders' own and the font is Nunito (SIL OFL 1.1, third_party/fonts/Nunito).
#include "overlay/overlay_ui.h"
#include "overlay/overlay_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <iterator>

#include "imgui_internal.h"  // ImGuiStorage of the current window

#include <nx/runtime.h>
#include "platform.h"

namespace nx::overlay::ui {

namespace {

// Nunito SemiBold and ExtraBold, in the program (no files to ship or find). #embed: C23, a Clang and GCC 15
// extension in C++.
#if defined(__has_embed)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#if __has_embed("../../../third_party/fonts/Nunito/Nunito-SemiBold.ttf")
#define NX_NUNITO 1
const unsigned char kBodyFont[] = {
#embed "../../../third_party/fonts/Nunito/Nunito-SemiBold.ttf"
};
const unsigned char kTitleFont[] = {
#embed "../../../third_party/fonts/Nunito/Nunito-ExtraBold.ttf"
};
#endif
#if __has_embed("../../../third_party/fonts/NotoFallback/NotoSansJP-menu.ttf")
#define NX_NOTO_MENU_JP 1
const unsigned char kNotoMenuJP[] = {
#embed "../../../third_party/fonts/NotoFallback/NotoSansJP-menu.ttf"
};
#endif
#if __has_embed("../../../third_party/fonts/NotoFallback/NotoSansKR-menu.ttf")
#define NX_NOTO_MENU_KR 1
const unsigned char kNotoMenuKR[] = {
#embed "../../../third_party/fonts/NotoFallback/NotoSansKR-menu.ttf"
};
#endif
#if __has_embed("../../../third_party/fonts/NotoFallback/NotoSansSC-menu.ttf")
#define NX_NOTO_MENU_SC 1
const unsigned char kNotoMenuSC[] = {
#embed "../../../third_party/fonts/NotoFallback/NotoSansSC-menu.ttf"
};
#endif
#if __has_embed("../../../third_party/fonts/NotoFallback/NotoSans-menu.ttf")
#define NX_NOTO_MENU_SANS 1
const unsigned char kNotoMenuSans[] = {
#embed "../../../third_party/fonts/NotoFallback/NotoSans-menu.ttf"
};
#endif
#if __has_embed("../../../third_party/fonts/NotoFallback/NotoSansArabic-menu.ttf")
#define NX_NOTO_MENU_ARABIC 1
const unsigned char kNotoMenuArabic[] = {
#embed "../../../third_party/fonts/NotoFallback/NotoSansArabic-menu.ttf"
};
#endif
#pragma clang diagnostic pop
#endif

// Base-font fallbacks (the in-game menu also merges script glyphs from OS fonts below).
const char* const kSystemFonts[] = {
#if defined(_WIN32)
    "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJKkr-Regular.otf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
};

// Baked large and drawn smaller through FontGlobalScale (sharp up to 4K).
constexpr float kBodyBake = 40.0f, kTitleBake = 72.0f;
constexpr float kBody1080 = 29.0f;  // the menu rows at 1080 p (Savage Wind's size)

ImFont* g_body = nullptr;
ImFont* g_title = nullptr;

// Latin-1 plus menu punctuation. Script glyphs used by the in-game menu and launcher are merged below.
const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201E, 0x2022, 0x2026, 0x2190, 0x2193, 0};
const char kLocalizedGlyphs[] = "ÉÖÜßàáäçèéêíñóöùúüğışАБВГЗИКМОПРСУЭЯабвгдежзийклмнопрстуфхцчшыьэюяءأإئابةتجحخدذرزسشصضطظعغفقكلمنهوىيًُ٪—’、。いがきさしじすたでにのはまるれをァアィイウエォオキクグケゲコサシジスズタチッテトドナニネバパフブプボマミムメモャュラリルレロンー上下了位低作値像全内再出前力動右同声変定左常度後必性手振操放方日映時更期本用画異的示終置表要解言設語起速適部配量閉間限面音간거게국그기내너네뉴는능니다닫도동됩드등디또래량러레려력로롤료른를리링마메면명모방배버보본부브비빠상선설성세소속스시십싱아안앨어언업에오요용원위을음이일임자작재저적전정제조종준진쪽창처체출치컨케크키터텍템트튼티표프플필하한항해화후PlayGme fisStngModUpQurWAbYExFRCh.cOBkVHDvwT'Izq-KZLJjÀ파데폴더택현서사복열새고침완코빌야합되지않뒤페먼홈라법취新概ダ選択こ場所使うピセデ開く準備完ビてだ未戻ペ始先ョホそ他法ثФНДÇİ";

const char kAdditionalLocalizedGlyphs[] = "简体繁體快速设置视频音频控制性能叠加层窗口输出大小内部解析度全屏窗口置顶游戏音量静音游戏语言重新启动游戏以应用语言按键布局下方右侧帧率上限系统内存清理阈值低显存配置纹理显示画面帧同步升频锐化抗锯齿各向异性过滤垂直同步震动退出菜单语言帧时间图表位置键盘控制器原始动态屏幕原生快速設定影片音訊效能覆蓋層視窗輸出大小內部解析度全螢幕遊戲音量靜音遊戲語言重新啟動遊戲以套用語言按鍵配置影格率上限系統記憶體清理閾值低顯示記憶體設定材質顯示畫面升頻反鋸齒過濾垂直同步震動結束選單語言影格時間圖表鍵盤控制器原始動態螢幕原生";
const char kLauncherLocalizedGlyphs[] = "开始文件模组更新退出";
// Korean characters used in the help descriptions as well as the menu labels.
const char kKoreanHelpGlyphs[] = "가값강계과근꾸끄끕널넘높누느닙단당대된두듬딩록릅린묘미바번범분수습식압약없와유으은의인입있장절접줄중증직질집찢초최축충패할행확환황흐히";
const char kAdditionalHelpGlyphs[] = "三十叉圆圓块塊字角왼향،ヤ一个从入別备家從无機独獨玩立続線自裝连連결끔됨르별연켬ТЧщёآあえかずせちとどなほぼみめもらりわんギザ不与丢两中为也乱会保個側儲兩关减分切利効卡即压取变可号合后和回围圍圧増壓多够夠失対将將少幀平式強强微応慢或押拡择持换接提換撕擇改数整數斜明是晰替最會板柄染案檔檻次正此毎每毫況消済減渲滑為無状现現电省码确確碼秒稍算節範糊約素細組緣縮繪细缘缩耗脑腦至與节范荷號行裂見認調請變认请调負超足軽較輪较辨边近这进选這通進遅遺邊郭释釋銳門關闭離電需頂高鮮؛げっびへよハ亮伸余光其割参參収地场基增外太尼屋峰應截房援支断斷早晕景暈校样樣残测測点百程紙约纸终考詰越趋趨輝長长閃闪阳陽非餘點갑같꺼뀌나남납낮낼늘던럼럽렴며반밝백빛았앱얼여올잘점져주짝톤；";
const char kExtraLocalizedGlyphs[] = "ؤ〜ノ之例具在宽寬広果標比环環相离繞绕聲道노양우운율좌커테퍼폭피효（），：Цّべカ主介初厂复已廠復恢有界色頁預題项题默눌돌든런립색었타탭《》ワ兄奇弟惊欧歐狀瑪級级驚马태けづやガ丰于仅伽僅含呈室富对對形彩心急感拟擬於暗曝曲来沈深満灰留目知空緩线缓般覚觀见观讓让豊豐适阴陡除陰露飽饱馬黑黒각감곡공깊까낌냅둡림만외워채풍형회흉흑";
const char kSpeedrunLocalizedGlyphs[] = "ÇßàáäçèéñóöùúüğİışАБВГЗКМНОПРСТЦабвгдежзийклмнопрстуфхцчшыьэюяёأؤئابةتجحخدذرسشصضطظعغفقكلمنهوىي、。いがきくさしすただてでとにのはまるれをアイオクゲコゴサシスズセタットドニピブプマムメモュョリルン・ー上下両中以位停入內关内准出分动効動单卡右同启啟單器在大央始字实実實小屏左已幕式戏或戲択择擇数整數方时显時暂暫有标格標模止游準無特用画白的真示禁粉紅綠緑红绿置自色菜螢表計設調請计请调赤超进退选進遊選重金開間關间離青面頁項顏顯项颜風风가간강게고금기내뉴는니다단동되됩둘드또려로록를매머메면모보분비빨상색서선성세션숫스습시실앙어에옵요우위으을의이일임입있자작장절정제조좌중지청초측치켜코크타택탭통퇴표하할합홍화활흰，";

const char* const kScriptFonts[] = {
#if defined(_WIN32)
    "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/meiryo.ttc",
    "C:/Windows/Fonts/msgothic.ttc", "C:/Windows/Fonts/YuGothR.ttc", "C:/Windows/Fonts/malgun.ttf",
    "C:/Windows/Fonts/gulim.ttc", "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/mingliu.ttc",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
#else
    "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJKkr-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
};

struct Notice {
    std::string text;
    uint64_t at_ns;
};
std::mutex g_notice_lock;
std::deque<Notice> g_notices;
constexpr double kNoticeSeconds = 3.2;

}  // namespace

ImU32 fade(ImU32 c, float a) { return ImGui::GetColorU32(c, std::clamp(a, 0.0f, 1.0f)); }

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 A = ImGui::ColorConvertU32ToFloat4(a), B = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(
        {A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t, A.z + (B.z - A.z) * t, A.w + (B.w - A.w) * t});
}

namespace {
bool theme_is_light();
}

ImU32 tint(int alpha) {
    if (!theme_is_light()) return IM_COL32(255, 255, 255, std::clamp(alpha, 0, 255));
    return IM_COL32(30, 52, 120, std::clamp(int(alpha * 1.6f), 0, 255));
}

void draw_card(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, float k, ImU32 fill, ImU32 outline, float alpha) {
    for (int i = 2; i >= 0; --i) {  // a soft shadow: three growing, faint layers
        const float e = float(i) * 5 * k;
        dl->AddRectFilled({p0.x - e, p0.y + 6 * k - e}, {p1.x + e, p1.y + 10 * k + e},
                          fade(IM_COL32(20, 50, 110, 24), alpha), rounding + e);
    }
    dl->AddRectFilled(p0, p1, fill, rounding);
    dl->AddRect(p0, p1, outline, rounding, 0, std::max(1.0f, 3.0f * k));
}

void draw_band(ImDrawList* dl, ImVec2 p0, float width, float height, float rounding, float k, ImU32 colour) {
    dl->AddRectFilled(p0, {p0.x + width, p0.y + height}, colour, rounding, ImDrawFlags_RoundCornersTop);
    const float r = 10 * k;  // a row of round scallops along the lower edge
    for (float x = p0.x + rounding; x <= p0.x + width - rounding; x += r * 1.9f)
        dl->AddCircleFilled({x, p0.y + height}, r, colour, 16);
    dl->AddRectFilled({p0.x + rounding, p0.y + 7 * k}, {p0.x + width - rounding, p0.y + 11 * k},
                      mix(colour, IM_COL32(255, 255, 255, 255), 0.28f), 2 * k);  // (a soft highlight)
}

void draw_sky(ImDrawList* dl, ImVec2 ds, float k, float alpha) {
    const Theme& th = theme();
    const float t = float(ImGui::GetTime());
    const ImU32 top = fade(th.back_top, alpha), bottom = fade(th.back_mid, alpha);
    dl->AddRectFilledMultiColor({0, 0}, ds, top, top, bottom, bottom);
    {  // a soft sun: a lit centre fading to clear
        const float gx = ds.x * 0.84f, gy = ds.y * 0.17f, r = ds.y * 0.42f;
        const ImU32 in = fade(IM_COL32(255, 248, 205, 150), alpha), out = in & ~IM_COL32_A_MASK;
        constexpr int kSeg = 48;
        dl->PrimReserve(kSeg * 3, kSeg + 1);
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
        dl->PrimWriteVtx({gx, gy}, uv, in);
        for (int i = 0; i < kSeg; ++i) {
            const float a = float(i) / kSeg * 6.2831853f;
            dl->PrimWriteVtx({gx + std::cos(a) * r, gy + std::sin(a) * r}, uv, out);
        }
        for (int i = 0; i < kSeg; ++i) {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % kSeg));
        }
    }
    {  // a faint dot pattern, in staggered rows
        const float sp = 38 * k, rr = 2.2f * k;
        const ImU32 dot = fade(IM_COL32(255, 255, 255, 44), alpha);
        int row = 0;
        for (float y = sp * 0.5f; y < ds.y; y += sp, ++row)
            for (float x = (row & 1 ? sp : sp * 0.5f); x < ds.x; x += sp) dl->AddCircleFilled({x, y}, rr, dot, 6);
    }
    {  // clouds drifting right, each from overlapping round puffs on a flat base
        struct Cloud { float y, scale, speed, phase; };
        static const Cloud clouds[] = {{0.14f, 1.0f, 14, 0.05f}, {0.30f, 0.7f, 9, 0.45f}, {0.52f, 1.25f, 11, 0.72f},
                                       {0.08f, 0.6f, 7, 0.90f}, {0.42f, 0.8f, 16, 0.20f}};
        const ImU32 white = fade(IM_COL32(255, 255, 255, 205), alpha);
        const float span = ds.x + 600 * k;
        for (const Cloud& c : clouds) {
            const float cx = std::fmod(c.phase * span + t * c.speed * k, span) - 300 * k, cy = ds.y * c.y;
            const float w = 220 * k * c.scale;
            dl->AddRectFilled({cx - w * 0.5f, cy}, {cx + w * 0.5f, cy + w * 0.22f}, white, w * 0.11f);
            dl->AddCircleFilled({cx - w * 0.22f, cy + w * 0.02f}, w * 0.17f, white, 20);
            dl->AddCircleFilled({cx, cy - w * 0.08f}, w * 0.23f, white, 24);
            dl->AddCircleFilled({cx + w * 0.24f, cy + w * 0.02f}, w * 0.16f, white, 20);
        }
    }
    {  // two rolling green hills along the bottom
        auto hill = [&](float base, float amp, float freq, float phase, ImU32 body, ImU32 edge) {
            const float step = 10 * k;
            float px = 0, py = ds.y * base + amp * std::sin(phase);
            for (float x = step; x <= ds.x + step; x += step) {
                const float y = ds.y * base + amp * std::sin(x * freq + phase);
                dl->AddQuadFilled({px, py}, {x, y}, {x, ds.y}, {px, ds.y}, body);
                dl->AddLine({px, py}, {x, y}, edge, std::max(1.0f, 4 * k));
                px = x, py = y;
            }
        };
        hill(0.89f, 22 * k, 0.0042f / k, 0.6f, fade(IM_COL32(126, 214, 118, 255), alpha), fade(IM_COL32(168, 232, 146, 255), alpha));
        hill(0.94f, 16 * k, 0.0061f / k, 2.4f, fade(IM_COL32(72, 182, 86, 255), alpha), fade(IM_COL32(112, 208, 104, 255), alpha));
    }
}

namespace {

// Palettes. The first, "Super Sky", is the house look: bright and playful (sky blue, a cheerful red, sunny yellow
// touches, white cards, chunky rounded shapes), drawn entirely from shapes and colours of our own, with no pictures
// from any game. The others are modern minimalist dark themes: neutral surfaces with refined accents.
const Theme kThemes[] = {
    {"Super Sky", IM_COL32(230, 40, 48, 255), IM_COL32(0, 150, 235, 255), IM_COL32(26, 38, 82, 255),
     IM_COL32(86, 100, 146, 255), IM_COL32(158, 168, 198, 255), IM_COL32(252, 253, 255, 246),
     IM_COL32(232, 242, 255, 252), IM_COL32(86, 176, 255, 255), IM_COL32(184, 226, 255, 255),
     IM_COL32(24, 168, 92, 255), IM_COL32(232, 146, 0, 255), IM_COL32(224, 36, 48, 255), true},
#if defined(NX_GAME_NAME)
    {"Midnight Gold", IM_COL32(248, 192, 48, 255), IM_COL32(255, 224, 112, 255), IM_COL32(244, 247, 252, 255),
     IM_COL32(176, 186, 208, 255), IM_COL32(108, 120, 148, 255), IM_COL32(15, 22, 42, 242), IM_COL32(22, 32, 60, 252),
     IM_COL32(9, 14, 30, 255), IM_COL32(15, 24, 52, 255), IM_COL32(74, 222, 128, 255), IM_COL32(250, 204, 21, 255),
     IM_COL32(248, 113, 113, 255)},
#endif
    {"Obsidian", IM_COL32(244, 91, 147, 255), IM_COL32(122, 200, 255, 255), IM_COL32(244, 246, 250, 255),
     IM_COL32(174, 180, 194, 255), IM_COL32(106, 112, 128, 255), IM_COL32(18, 20, 26, 242), IM_COL32(25, 28, 36, 252),
     IM_COL32(11, 13, 18, 255), IM_COL32(18, 21, 29, 255), IM_COL32(74, 222, 128, 255), IM_COL32(250, 204, 21, 255),
     IM_COL32(248, 113, 113, 255)},
    {"Cobalt", IM_COL32(79, 158, 255, 255), IM_COL32(130, 217, 255, 255), IM_COL32(244, 247, 252, 255),
     IM_COL32(172, 184, 204, 255), IM_COL32(104, 116, 138, 255), IM_COL32(15, 20, 29, 242), IM_COL32(22, 29, 42, 252),
     IM_COL32(10, 14, 21, 255), IM_COL32(16, 23, 35, 255), IM_COL32(74, 222, 128, 255), IM_COL32(250, 204, 21, 255),
     IM_COL32(248, 113, 113, 255)},
    {"Emerald", IM_COL32(52, 211, 153, 255), IM_COL32(124, 236, 196, 255), IM_COL32(243, 248, 246, 255),
     IM_COL32(170, 188, 180, 255), IM_COL32(102, 120, 112, 255), IM_COL32(16, 22, 20, 242), IM_COL32(22, 32, 28, 252),
     IM_COL32(10, 15, 13, 255), IM_COL32(16, 25, 21, 255), IM_COL32(74, 222, 128, 255), IM_COL32(250, 204, 21, 255),
     IM_COL32(248, 113, 113, 255)},
    {"Amber", IM_COL32(245, 158, 11, 255), IM_COL32(252, 211, 77, 255), IM_COL32(248, 246, 242, 255),
     IM_COL32(188, 180, 168, 255), IM_COL32(120, 114, 104, 255), IM_COL32(21, 19, 17, 242), IM_COL32(30, 27, 23, 252),
     IM_COL32(14, 12, 10, 255), IM_COL32(23, 20, 16, 255), IM_COL32(74, 222, 128, 255), IM_COL32(250, 204, 21, 255),
     IM_COL32(248, 113, 113, 255)},
    {"Crimson", IM_COL32(239, 68, 68, 255), IM_COL32(252, 165, 165, 255), IM_COL32(246, 246, 248, 255),
     IM_COL32(180, 180, 188, 255), IM_COL32(112, 112, 120, 255), IM_COL32(18, 18, 21, 242), IM_COL32(26, 26, 32, 252),
     IM_COL32(11, 11, 14, 255), IM_COL32(20, 18, 22, 255), IM_COL32(74, 222, 128, 255), IM_COL32(250, 204, 21, 255),
     IM_COL32(248, 113, 113, 255)},
};
std::atomic<int> g_theme_index{0};

bool theme_is_light() { return theme().light; }

}  // namespace

const Theme& theme() {
    const int idx = std::clamp(g_theme_index.load(std::memory_order_relaxed), 0, int(std::size(kThemes)) - 1);
    return kThemes[idx];
}
int theme_count() { return int(std::size(kThemes)); }
int theme_index() { return std::clamp(g_theme_index.load(std::memory_order_relaxed), 0, int(std::size(kThemes)) - 1); }
const char* theme_name(int index) { return kThemes[std::clamp(index, 0, int(std::size(kThemes)) - 1)].name; }
void set_theme(int index) { g_theme_index.store(std::clamp(index, 0, int(std::size(kThemes)) - 1), std::memory_order_relaxed); }
void cycle_theme() { set_theme((theme_index() + 1) % theme_count()); }

bool theme_button(float k, bool on_accent) {
    const Theme& th = theme();
    ImFont* f = body();
    const float fs = ImGui::GetFontSize() * 0.84f;
    const float h = 38 * k;
    const float dot_r = 5.5f * k;
    const float pad_x = 14 * k;
    const float gap = 8 * k;
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, th.name);
    const float w = pad_x * 2 + dot_r * 2 + gap + ts.x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::InvisibleButton("##theme_btn", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible);
    const float t = eased(ImGui::GetItemID(), hot ? 1.0f : 0.0f, 22.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    const float r = h * 0.5f;
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    if (on_accent) {  // over the coloured header band: white on translucent white
        dl->AddRectFilled(p, q, fade(mix(IM_COL32(255, 255, 255, 40), IM_COL32(255, 255, 255, 70), t)), r);
        dl->AddRect(p, q, fade(mix(IM_COL32(255, 255, 255, 120), white, t)), r, 0, std::max(1.0f, 1.6f * k));
        dl->AddCircleFilled(ImVec2(p.x + pad_x + dot_r, p.y + h * 0.5f), dot_r, fade(white));
        dl->AddText(f, fs, ImVec2(p.x + pad_x + dot_r * 2 + gap, p.y + (h - ts.y) * 0.5f), fade(white), th.name);
    } else {
        dl->AddRectFilled(p, q, fade(mix(tint(14), tint(28), t)), r);
        dl->AddRect(p, q, fade(mix(tint(32), th.accent, t)), r, 0, std::max(1.0f, 1.2f * k));
        dl->AddCircleFilled(ImVec2(p.x + pad_x + dot_r, p.y + h * 0.5f), dot_r, fade(th.accent));
        dl->AddText(f, fs, ImVec2(p.x + pad_x + dot_r * 2 + gap, p.y + (h - ts.y) * 0.5f),
                    fade(mix(th.text_dim, th.text, t)), th.name);
    }
    if (clicked) {
        cycle_theme();
        return true;
    }
    return false;
}

namespace {
ImVec4 v4(ImU32 c, float a = -1) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    if (a >= 0) v.w = a;
    return v;
}
}  // namespace

void apply_style(float height) {
    static float made_for = -1;
    static int made_theme = -1;
    const float k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    const int cur_theme = theme_index();
    if (std::fabs(made_for - k) < 0.005f && made_theme == cur_theme) return;
    made_for = k;
    made_theme = cur_theme;
    const Theme& t = theme();
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 16, s.FrameRounding = 10, s.GrabRounding = 8, s.TabRounding = 8, s.ChildRounding = 12;
    s.PopupRounding = 12, s.ScrollbarRounding = 8;
    s.WindowPadding = {32, 26}, s.FramePadding = {14, 8}, s.ItemSpacing = {14, 10}, s.ItemInnerSpacing = {10, 8};
    s.WindowBorderSize = 1, s.PopupBorderSize = 1, s.ChildBorderSize = 0, s.TabBorderSize = 0;
    s.ScrollbarSize = 8, s.TabBarBorderSize = 1;
    if (t.light) {  // chunkier and rounder: pill-like controls, a thick outline around windows and popups
        s.WindowRounding = 30, s.FrameRounding = 16, s.GrabRounding = 12, s.TabRounding = 16, s.ChildRounding = 20;
        s.PopupRounding = 20, s.ScrollbarRounding = 10;
        s.WindowBorderSize = 3, s.PopupBorderSize = 3, s.ScrollbarSize = 10;
    }
    s.DisabledAlpha = 0.42f;
    ImVec4* c = s.Colors;
    const ImVec4 accent = v4(t.accent), lift = v4(mix(t.panel, t.text, 0.08f), 1), lift2 = v4(mix(t.panel, t.text, 0.14f), 1);
    c[ImGuiCol_WindowBg] = v4(t.panel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v4(t.card, 0.98f);
    c[ImGuiCol_Border] = t.light ? v4(t.accent2, 0.9f) : ImVec4(1, 1, 1, 0.09f);
    c[ImGuiCol_Text] = v4(t.text);
    c[ImGuiCol_TextDisabled] = v4(t.text_off);
    c[ImGuiCol_FrameBg] = v4(mix(t.panel, t.text, 0.05f), 1);
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_Header] = lift;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = lift2;
    c[ImGuiCol_Button] = v4(mix(t.panel, t.text, 0.06f), 1);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = v4(tint(15));
    c[ImGuiCol_TabSelected] = v4(tint(13));
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);  // (the menu draws its own sliding underline)
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab], c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_NavCursor] = c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_ScrollbarGrabActive] = v4(t.accent, 0.7f);
    c[ImGuiCol_Separator] = v4(tint(20));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v4(tint(36));
    c[ImGuiCol_ScrollbarGrabHovered] = v4(tint(60));
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    s.ScaleAllSizes(k);
}

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_body = g_title = nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
#ifdef NX_NUNITO
    cfg.FontDataOwnedByAtlas = false;  // (static data)
    g_body = io.Fonts->AddFontFromMemoryTTF((void*)kBodyFont, int(sizeof kBodyFont), kBodyBake, &cfg, kRanges);
    g_title = io.Fonts->AddFontFromMemoryTTF((void*)kTitleFont, int(sizeof kTitleFont), kTitleBake, &cfg, kRanges);
    if (g_body) nx::log("overlay: font Nunito (embedded)");
#endif
    cfg.FontDataOwnedByAtlas = true;
    for (const char* path : kSystemFonts) {
        if (g_body) break;
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            g_body = io.Fonts->AddFontFromFileTTF(path, kBodyBake, &cfg, kRanges);
            if (g_body) g_title = io.Fonts->AddFontFromFileTTF(path, kTitleBake, &cfg, kRanges), nx::log("overlay: font %s", path);
        }
    }
    if (!g_body) {
        ImFontConfig c;
        c.SizePixels = kBodyBake;
        g_body = io.Fonts->AddFontDefault(&c);
    }
    if (!g_title) g_title = g_body;
    ImFontGlyphRangesBuilder glyph_builder;
    glyph_builder.AddText(kLocalizedGlyphs);
    glyph_builder.AddText(kAdditionalLocalizedGlyphs);
    glyph_builder.AddText(kLauncherLocalizedGlyphs);
    glyph_builder.AddText(kKoreanHelpGlyphs);
    glyph_builder.AddText(kAdditionalHelpGlyphs);
    glyph_builder.AddText(kExtraLocalizedGlyphs);
    glyph_builder.AddText(kSpeedrunLocalizedGlyphs);
    // ImGui keeps this pointer in the font config and consumes it later, when the
    // Vulkan backend builds the atlas on its first NewFrame. Keep it alive beyond
    // this function; a local vector here leaves every merged fallback with a
    // dangling glyph-range pointer.
    static ImVector<ImWchar> localized_ranges;
    localized_ranges.clear();
    glyph_builder.BuildRanges(&localized_ranges);
#if defined(NX_NOTO_MENU_JP) || defined(NX_NOTO_MENU_KR) || defined(NX_NOTO_MENU_SC) || defined(NX_NOTO_MENU_SANS) || defined(NX_NOTO_MENU_ARABIC)
    ImFontConfig bundled;
    bundled.MergeMode = true;
    bundled.FontDataOwnedByAtlas = false;  // embedded static data; ImGui copies it into its atlas
    bundled.OversampleH = 2;
#ifdef NX_NOTO_MENU_JP
    bundled.DstFont = g_body;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuJP, int(sizeof kNotoMenuJP), kBodyBake, &bundled,
                                   localized_ranges.Data);
    bundled.DstFont = g_title;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuJP, int(sizeof kNotoMenuJP), kTitleBake, &bundled,
                                   localized_ranges.Data);
    nx::log("overlay: embedded Noto Sans JP glyph fallback");
#endif
#ifdef NX_NOTO_MENU_KR
    bundled.DstFont = g_body;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuKR, int(sizeof kNotoMenuKR), kBodyBake, &bundled,
                                   localized_ranges.Data);
    bundled.DstFont = g_title;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuKR, int(sizeof kNotoMenuKR), kTitleBake, &bundled,
                                   localized_ranges.Data);
    nx::log("overlay: embedded Noto Sans KR glyph fallback");
#endif
#ifdef NX_NOTO_MENU_SC
    bundled.DstFont = g_body;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuSC, int(sizeof kNotoMenuSC), kBodyBake, &bundled,
                                   localized_ranges.Data);
    bundled.DstFont = g_title;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuSC, int(sizeof kNotoMenuSC), kTitleBake, &bundled,
                                   localized_ranges.Data);
    nx::log("overlay: embedded Noto Sans SC glyph fallback");
#endif
#ifdef NX_NOTO_MENU_SANS
    bundled.DstFont = g_body;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuSans, int(sizeof kNotoMenuSans), kBodyBake, &bundled,
                                   localized_ranges.Data);
    bundled.DstFont = g_title;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuSans, int(sizeof kNotoMenuSans), kTitleBake, &bundled,
                                   localized_ranges.Data);
    nx::log("overlay: embedded Noto Sans glyph fallback");
#endif
#ifdef NX_NOTO_MENU_ARABIC
    bundled.DstFont = g_body;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuArabic, int(sizeof kNotoMenuArabic), kBodyBake, &bundled,
                                   localized_ranges.Data);
    bundled.DstFont = g_title;
    io.Fonts->AddFontFromMemoryTTF((void*)kNotoMenuArabic, int(sizeof kNotoMenuArabic), kTitleBake, &bundled,
                                   localized_ranges.Data);
    nx::log("overlay: embedded Noto Sans Arabic glyph fallback");
#endif
#endif
    for (const char* path : kScriptFonts) {
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            ImFontConfig fallback;
            fallback.MergeMode = true;
            fallback.FontNo = 0;  // some CJK system fonts are collections
            fallback.OversampleH = 2;
            fallback.DstFont = g_body;
            io.Fonts->AddFontFromFileTTF(path, kBodyBake, &fallback, localized_ranges.Data);
            fallback.DstFont = g_title;
            io.Fonts->AddFontFromFileTTF(path, kTitleBake, &fallback, localized_ranges.Data);
        }
    }
    io.FontDefault = g_body;
}

ImFont* body() { return g_body; }
ImFont* title() { return g_title; }

float font_scale(float height) { return kBody1080 * std::clamp(height / 1080.0f, 0.5f, 3.0f) / kBodyBake; }

float approach(float current, float target, float rate) {
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    const float t = 1.0f - std::exp(-rate * dt);
    const float next = current + (target - current) * t;
    return std::fabs(target - next) < 0.002f ? target : next;
}

float eased(ImGuiID id, float target, float rate) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float* v = st->GetFloatRef(id, target);
    *v = approach(*v, target, rate);
    return *v;
}

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// ---- notices ----

struct ShotToast {
    bool active = false;
    bool pending_upload = false;
    uint64_t at_ns = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    uint64_t tex_id = 0;
};
ShotToast g_shot;
constexpr double kShotSeconds = 2.8;

void notify(const char* text) {
    if (!text || !*text) return;
    std::lock_guard lk(g_notice_lock);
    for (Notice& n : g_notices)  // the same notice again: shown longer, not twice
        if (n.text == text) {
            n.at_ns = plat::monotonic_ns();
            return;
        }
    g_notices.push_back({text, plat::monotonic_ns()});
    while (g_notices.size() > 3) g_notices.pop_front();
}

void notify_screenshot(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    std::lock_guard lk(g_notice_lock);
    g_shot.rgba.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
    g_shot.w = w;
    g_shot.h = h;
    g_shot.at_ns = plat::monotonic_ns();
    g_shot.pending_upload = true;
    g_shot.active = true;
}

bool notices_active() {
    std::lock_guard lk(g_notice_lock);
    const uint64_t now = plat::monotonic_ns();
    while (!g_notices.empty() && double(now - g_notices.front().at_ns) / 1e9 > kNoticeSeconds) g_notices.pop_front();
    if (g_shot.active && double(now - g_shot.at_ns) / 1e9 > kShotSeconds) g_shot.active = false;
    return !g_notices.empty() || g_shot.active;
}

// Savage Wind's "Picture saved" notice (menu.mm photo_overlay_draw): a dark card in the bottom-right corner that fades
// out over its last half second; here it also slides in from the right.
void draw_notices(float height) {
    std::deque<Notice> list;
    bool shot_active = false;
    uint64_t shot_at_ns = 0;
    int shot_w = 0, shot_h = 0;
    uint64_t shot_tex = 0;
    {
        std::lock_guard lk(g_notice_lock);
        list = g_notices;
        if (g_shot.pending_upload) {
            if (g_shot.tex_id) {
                vk_free_rgba_texture(g_shot.tex_id);
                g_shot.tex_id = 0;
            }
            if (!g_shot.rgba.empty() && g_shot.w > 0 && g_shot.h > 0)
                g_shot.tex_id = vk_upload_rgba_texture(g_shot.rgba.data(), g_shot.w, g_shot.h);
            g_shot.rgba.clear();
            g_shot.pending_upload = false;
        }
        if (g_shot.active) {
            shot_active = true;
            shot_at_ns = g_shot.at_ns;
            shot_w = g_shot.w;
            shot_h = g_shot.h;
            shot_tex = g_shot.tex_id;
        }
    }
    const float k = std::clamp(height / 1080.0f, 0.5f, 3.0f);
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = body();
    const uint64_t now = plat::monotonic_ns();

    if (shot_active && shot_w > 0 && shot_h > 0) {
        const float age = float(double(now - shot_at_ns) / 1e9);
        const float in = smoothstep(age / 0.24f);
        const float out = age > float(kShotSeconds - 0.30) ? 1.0f - smoothstep((age - float(kShotSeconds - 0.30)) / 0.30f) : 1.0f;
        const float slide = std::min(in, out);
        const float fs = ImGui::GetFontSize() * 0.92f;
        const float pad = 12 * k;
        const float thumb_h = 54 * k;
        const float thumb_w = std::clamp(thumb_h * float(shot_w) / float(shot_h), 54 * k, 128 * k);
        const char* label = "Screenshot Saved";
        const ImVec2 ts = f->CalcTextSizeA(fs, 320 * k, 320 * k, label);
        const float card_w = pad + 5 * k + thumb_w + 14 * k + ts.x + pad * 1.3f;
        const float card_h = thumb_h + pad * 2.0f;
        const float x = ds.x - 28 * k - card_w;
        const float y_rest = 24 * k;
        const float y_hidden = -card_h - 12 * k;
        const float y = y_hidden + (y_rest - y_hidden) * slide;
        const ImVec2 p0(x, y), p1(x + card_w, y + card_h);
        dl->AddRectFilled(p0, p1, fade(theme().card, slide * 0.96f), 10 * k);
        dl->AddRect(p0, p1, fade(theme().accent, slide * 0.45f), 10 * k, 0, std::max(1.0f, k));
        dl->AddRectFilled(p0, ImVec2(p0.x + 5 * k, p1.y), fade(theme().accent, slide), 10 * k, ImDrawFlags_RoundCornersLeft);
        const ImVec2 tp0(p0.x + pad + 4 * k, p0.y + pad);
        const ImVec2 tp1(tp0.x + thumb_w, tp0.y + thumb_h);
        if (shot_tex)
            dl->AddImageRounded(ImTextureID(uintptr_t(shot_tex)), tp0, tp1, {0, 0}, {1, 1}, fade(IM_COL32_WHITE, slide), 6 * k);
        else
            dl->AddRectFilled(tp0, tp1, fade(theme().panel, slide), 6 * k);
        dl->AddText(f, fs, ImVec2(tp1.x + 14 * k, p0.y + (card_h - ts.y) * 0.5f), fade(theme().text, slide), label);
    }

    if (list.empty()) return;
    const float fs = ImGui::GetFontSize() * 0.9f, pad = 16 * k;
    float y = ds.y - 28 * k;
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        const float age = float(double(now - it->at_ns) / 1e9);
        const float in = smoothstep(age / 0.22f);
        const float out = age > kNoticeSeconds - 0.5f ? 1.0f - smoothstep((age - (kNoticeSeconds - 0.5f)) / 0.5f) : 1.0f;
        const float a = std::min(in, out);
        const ImVec2 ts = f->CalcTextSizeA(fs, 640 * k, 640 * k, it->text.c_str());
        const float w = ts.x + 2 * pad + 10 * k, h = ts.y + 2 * pad * 0.8f;
        const float x = ds.x - 28 * k - w + (1.0f - in) * 40 * k;
        const ImVec2 p0(x, y - h), p1(x + w, y);
        dl->AddRectFilled(p0, p1, fade(theme().card, a * 0.95f), 10 * k);
        dl->AddRectFilled(p0, ImVec2(p0.x + 5 * k, p1.y), fade(theme().accent, a), 10 * k, ImDrawFlags_RoundCornersLeft);
        dl->AddText(f, fs, ImVec2(p0.x + pad + 5 * k, p0.y + pad * 0.8f), fade(theme().text, a), it->text.c_str(), nullptr,
                    640 * k);
        y -= h + 10 * k;
    }
}

// ---- controller hints ----

void progress_bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, float alpha) {
    const Theme& th = theme();
    const float h = b.y - a.y, r = h * 0.5f;
    dl->AddRectFilled(a, b, fade(tint(25), alpha), r);
    float fx = a.x + (b.x - a.x) * std::clamp(frac, 0.0f, 1.0f);
    if (fx <= a.x + 1) return;
    fx = std::max(fx, a.x + h);  // (at least its two round ends)
    dl->AddRectFilled(a, {fx, b.y}, fade(th.accent, alpha), r);
    // The sheen: a band a quarter of the filled part wide (at least 4 bar heights), the bar's full height. Its
    // brightness is worked out per slice, so it is soft in the middle, clear at its edges, and fades out over the round
    // ends instead of showing a square edge there.
    const float lo = a.x + r * 0.5f, hi = fx - r * 0.5f;
    if (hi - lo < 4) return;
    const float w = std::max(4 * h, (fx - a.x) * 0.25f);
    const float t = float(ImGui::GetTime());
    const float centre = lo - w * 0.5f + std::fmod(t / 1.6f, 1.0f) * (hi - lo + w);
    auto light = [&](float x) {
        const float band = std::max(0.0f, 1.0f - std::fabs(x - centre) / (w * 0.5f));
        const float ends = std::clamp(std::min(x - lo, hi - x) / r, 0.0f, 1.0f);
        return smoothstep(band) * smoothstep(ends);
    };
    const float x0 = std::max(lo, centre - w * 0.5f), x1 = std::min(hi, centre + w * 0.5f);
    if (x1 <= x0) return;
    constexpr int kSlices = 16;
    const ImU32 lit = IM_COL32(255, 248, 220, 120);
    for (int i = 0; i < kSlices; ++i) {
        const float sa = x0 + (x1 - x0) * float(i) / kSlices, sb = x0 + (x1 - x0) * float(i + 1) / kSlices;
        const ImU32 ca = fade(lit, alpha * light(sa)), cb = fade(lit, alpha * light(sb));
        // (full height inside the straight part; over the round ends the fade has it near clear already)
        const float inset_a = std::max(0.0f, r - std::min(sa - a.x, fx - sa)) * 0.9f;
        const float inset_b = std::max(0.0f, r - std::min(sb - a.x, fx - sb)) * 0.9f;
        const float inset = std::max(inset_a, inset_b);
        dl->AddRectFilledMultiColor({sa, a.y + inset}, {sb, b.y - inset}, ca, cb, cb, ca);
    }
}

float badge(ImDrawList* dl, ImVec2 pos, float h, const char* label, float alpha) {
    ImFont* f = body();
    const float fs = h * 0.62f;
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, label);
    const float w = std::max(h, ts.x + h * 0.6f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), fade(theme().text, alpha), h * 0.5f);
    dl->AddText(f, fs, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), fade(theme().panel | IM_COL32_A_MASK, alpha),
                label);
    return w;
}

void hints(ImDrawList* dl, float centre_x, float y, float h, const Hint* list, int n, float alpha) {
    ImFont* f = body();
    const float fs = h * 0.78f, gap = h * 0.35f, spacing = h * 1.1f;
    float total = 0;
    for (int i = 0; i < n; ++i) {
        float bw = 0;
        if (list[i].badge) {
            const ImVec2 ts = f->CalcTextSizeA(h * 0.62f, FLT_MAX, 0, list[i].badge);
            bw = std::max(h, ts.x + h * 0.6f) + gap;
        }
        total += bw + f->CalcTextSizeA(fs, FLT_MAX, 0, list[i].text).x + (i + 1 < n ? (*list[i].text ? spacing : 0) : 0);
    }
    float x = centre_x - total * 0.5f;
    for (int i = 0; i < n; ++i) {
        if (list[i].badge) x += badge(dl, ImVec2(x, y - h * 0.5f), h, list[i].badge, alpha) + gap;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, list[i].text);
        dl->AddText(f, fs, ImVec2(x, y - ts.y * 0.5f), fade(theme().text_dim, alpha), list[i].text);
        x += ts.x + (*list[i].text ? spacing : 0);  // (an empty text: the next badge belongs with this one)
    }
}

}  // namespace nx::overlay::ui
