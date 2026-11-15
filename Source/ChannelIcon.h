#pragma once

#include <JuceHeader.h>

#include "I18n.h"
#include "Theme.h"

//==============================================================================
// 通道预设图标：调音台名称左侧的小标识。
//
// · 图标以矢量线性路径自绘（与界面现有喇叭 / 键盘角标同一套风格），不依赖
//   位图资源；id 存入会话 / 预设（空串 = 默认无图标）。
// · 绘制函数同时供通道条名称左侧与「颜色与图标」选择面板的宫格预览复用。
//==============================================================================
namespace mb::ui
{
    namespace ChannelIcon
    {
        constexpr const char* Mic     = "mic";     // 麦克风
        constexpr const char* Vocal   = "vocal";   // 人声（人头）
        constexpr const char* Sing    = "sing";    // 唱歌（人 + 音符）
        constexpr const char* Track   = "track";   // 伴奏（连尾音符）
        constexpr const char* Guitar  = "guitar";  // 吉他
        constexpr const char* Bass    = "bass";    // 贝斯
        constexpr const char* Keys    = "keys";    // 钢琴 / 键盘
        constexpr const char* Synth   = "synth";   // 合成器
        constexpr const char* Drum    = "drum";   // 鼓
        constexpr const char* Speaker = "speaker"; // 监听音箱
        constexpr const char* Phones  = "phones";  // 监听耳机
        constexpr const char* System  = "system";  // 系统（显示器）
        constexpr const char* Out     = "out";     // 输出（出箱箭头）
        constexpr const char* Bus     = "bus";     // 总线（三合一汇流）
        constexpr const char* Pedal   = "pedal";   // 效果踏板
        constexpr const char* Bell    = "bell";    // 提示音（铃铛）
        constexpr const char* Play    = "play";    // 播放动作
        constexpr const char* Record  = "record"; // 录音动作
        constexpr const char* Fx      = "fx";     // 效果

        /** 可选预设图标 id 列表（5 列 × 4 行宫格：无图标格 + 19 预设）。 */
        inline juce::Array<juce::String> allIds()
        {
            return { Mic, Vocal, Sing, Track,
                     Guitar, Bass, Keys, Synth,
                     Drum, Speaker, Phones, System,
                     Out, Bus, Pedal, Bell,
                     Play, Record, Fx };
        }

        /** 图标中文 / 英文展示名（仅用于tooltip，宫格本身以图形为准）。 */
        inline juce::String label (const juce::String& id)
        {
            if (id == Mic)     return mb::tr ("麦克风", "Microphone");
            if (id == Vocal)   return mb::tr ("人声", "Vocal");
            if (id == Sing)    return mb::tr ("唱歌", "Singing");
            if (id == Track)   return mb::tr ("伴奏", "Backing track");
            if (id == Guitar)  return mb::tr ("吉他", "Guitar");
            if (id == Bass)    return mb::tr ("贝斯", "Bass");
            if (id == Keys)    return mb::tr ("键盘", "Keys");
            if (id == Synth)   return mb::tr ("合成器", "Synth");
            if (id == Drum)    return mb::tr ("鼓", "Drum");
            if (id == Speaker) return mb::tr ("音箱", "Speaker");
            if (id == Phones)  return mb::tr ("耳机", "Headphones");
            if (id == System)  return mb::tr ("系统", "System");
            if (id == Out)     return mb::tr ("输出", "Output");
            if (id == Bus)     return mb::tr ("总线", "Bus");
            if (id == Pedal)   return mb::tr ("效果踏板", "Stompbox");
            if (id == Bell)    return mb::tr ("提示音", "Bell");
            if (id == Play)    return mb::tr ("播放", "Play");
            if (id == Record)  return mb::tr ("录音", "Record");
            if (id == Fx)      return mb::tr ("效果", "FX");
            return {};
        }

        /** 在 r 内按线性 / 实心风格绘制指定图标；id 为空时不绘制。 */
        inline void draw (juce::Graphics& g, const juce::String& id,
                          juce::Rectangle<float> r, juce::Colour c)
        {
            if (id.isEmpty() || r.isEmpty()) return;

            const float cx = r.getCentreX(), cy = r.getCentreY();
            const juce::PathStrokeType st (1.5f, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded);
            auto stroke = [&] (const juce::Path& p)
            {
                g.setColour (c);
                g.strokePath (p, st);
            };
            auto fill = [&] (const juce::Path& p)
            {
                g.setColour (c);
                g.fillPath (p);
            };

            if (id == Mic)
            {
                // 麦克风头（圆角胶囊）
                juce::Path head;
                head.addRoundedRectangle (cx - 2.4f, cy - 6.0f, 4.8f, 7.0f, 2.4f);
                fill (head);
                // U 形支架
                juce::Path bracket;
                bracket.startNewSubPath (cx - 4.2f, cy - 1.0f);
                bracket.addCentredArc (cx, cy - 1.0f, 4.2f, 4.2f,
                                       juce::MathConstants<float>::pi,
                                       juce::MathConstants<float>::twoPi, false);
                bracket.lineTo (cx, cy + 3.2f);
                stroke (bracket);
                // 立杆 + 底座
                juce::Path stand;
                stand.startNewSubPath (cx, cy + 3.2f);
                stand.lineTo (cx, cy + 6.0f);
                stand.startNewSubPath (cx - 2.6f, cy + 6.2f);
                stand.lineTo (cx + 2.6f, cy + 6.2f);
                stroke (stand);
            }
            else if (id == Guitar)
            {
                // 琴身：上小下大双圆「8 字形」轮廓（竖置，琴颈居中在上）
                juce::Path body;
                body.addEllipse (cx - 3.2f, cy - 0.4f, 6.4f, 6.8f);   // 下部大圆
                body.addEllipse (cx - 2.2f, cy - 3.0f, 4.4f, 4.6f);   // 上部小圆
                fill (body);
                // 音孔
                juce::Path hole;
                hole.addEllipse (cx - 1.1f, cy + 0.6f, 2.2f, 2.2f);
                g.setColour (mb::ui::slotBg);
                g.fillPath (hole);
                // 琴颈（居中竖直向上，区别于音符的斜旗）
                juce::Path neck;
                neck.addRectangle (cx - 0.8f, cy - 6.8f, 1.6f, 4.2f);
                fill (neck);
            }
            else if (id == Keys)
            {
                // 琴键外框
                juce::Path frame;
                frame.addRectangle (cx - 6.0f, cy - 4.0f, 12.0f, 8.0f);
                stroke (frame);
                // 黑键：三个实心小方块自顶部垂下（比分隔线更像钢琴）
                juce::Path blacks;
                blacks.addRectangle (cx - 4.6f, cy - 4.0f, 1.8f, 4.6f);
                blacks.addRectangle (cx - 0.9f, cy - 4.0f, 1.8f, 4.6f);
                blacks.addRectangle (cx + 2.8f, cy - 4.0f, 1.8f, 4.6f);
                fill (blacks);
            }
            else if (id == Drum)
            {
                // 鼓面
                juce::Path top;
                top.addEllipse (cx - 5.0f, cy - 4.0f, 10.0f, 3.2f);
                stroke (top);
                // 鼓身
                juce::Path shell;
                shell.startNewSubPath (cx - 5.0f, cy - 2.6f);
                shell.lineTo (cx - 3.6f, cy + 5.0f);
                shell.lineTo (cx + 3.6f, cy + 5.0f);
                shell.lineTo (cx + 5.0f, cy - 2.6f);
                stroke (shell);
                // 鼓棒
                juce::Path sticks;
                sticks.startNewSubPath (cx - 5.0f, cy - 6.0f);
                sticks.lineTo (cx - 1.0f, cy - 3.0f);
                sticks.startNewSubPath (cx + 5.0f, cy - 6.0f);
                sticks.lineTo (cx + 1.0f, cy - 3.0f);
                stroke (sticks);
            }
            else if (id == Vocal)
            {
                // 人头
                juce::Path head;
                head.addEllipse (cx - 2.8f, cy - 5.6f, 5.6f, 5.6f);
                fill (head);
                // 肩部
                juce::Path shoulders;
                shoulders.startNewSubPath (cx - 5.4f, cy + 6.4f);
                shoulders.addCentredArc (cx, cy + 6.4f, 5.4f, 4.6f,
                                         juce::MathConstants<float>::pi,
                                         juce::MathConstants<float>::twoPi, false);
                shoulders.closeSubPath();
                fill (shoulders);
            }
            else if (id == Sing)
            {
                // 唱歌：人（头 + 肩，整体靠左）+ 右上音符
                juce::Path head;
                head.addEllipse (cx - 5.2f, cy - 5.2f, 4.6f, 4.6f);
                fill (head);
                juce::Path shoulders;
                shoulders.startNewSubPath (cx - 5.8f, cy + 6.4f);
                shoulders.addCentredArc (cx - 2.2f, cy + 6.4f, 3.6f, 3.2f,
                                         juce::MathConstants<float>::pi,
                                         juce::MathConstants<float>::twoPi, false);
                shoulders.closeSubPath();
                fill (shoulders);
                // 音符（符头 + 符干 + 小旗钩）
                juce::Path noteHead;
                noteHead.addEllipse (cx + 1.6f, cy + 2.4f, 2.6f, 2.0f);
                fill (noteHead);
                juce::Path stem;
                stem.startNewSubPath (cx + 4.0f, cy + 3.2f);
                stem.lineTo (cx + 4.0f, cy - 4.8f);
                stem.lineTo (cx + 5.6f, cy - 3.4f);
                stroke (stem);
            }
            else if (id == Track)
            {
                // 伴奏：连尾八分音符（两符头 + 双符干 + 横梁）
                juce::Path heads;
                heads.addEllipse (cx - 4.6f, cy + 2.6f, 3.0f, 2.4f);
                heads.addEllipse (cx + 0.6f, cy + 1.4f, 3.0f, 2.4f);
                fill (heads);
                juce::Path stems;
                stems.startNewSubPath (cx - 1.7f, cy + 3.6f);
                stems.lineTo (cx - 1.7f, cy - 4.2f);
                stems.startNewSubPath (cx + 3.5f, cy + 2.4f);
                stems.lineTo (cx + 3.5f, cy - 5.4f);
                stroke (stems);
                juce::Path beam;
                beam.startNewSubPath (cx - 1.7f, cy - 4.2f);
                beam.lineTo (cx + 3.5f, cy - 5.4f);
                g.setColour (c);
                g.strokePath (beam, juce::PathStrokeType (2.2f));
            }
            else if (id == Phones)
            {
                // 头梁拱（二次贝塞尔显式绘制，避免 addCentredArc 参数错位退化成一条线）
                juce::Path band;
                band.startNewSubPath (cx - 5.2f, cy + 0.8f);
                band.quadraticTo (cx - 5.2f, cy - 5.0f, cx, cy - 5.0f);
                band.quadraticTo (cx + 5.2f, cy - 5.0f, cx + 5.2f, cy + 0.8f);
                stroke (band);
                // 耳罩
                juce::Path pads;
                pads.addRoundedRectangle (cx - 6.4f, cy - 0.5f, 2.4f, 4.6f, 1.0f);
                pads.addRoundedRectangle (cx + 4.0f, cy - 0.5f, 2.4f, 4.6f, 1.0f);
                fill (pads);
            }
            else if (id == Play)
            {
                juce::Path tri;
                tri.startNewSubPath (cx - 2.6f, cy - 4.6f);
                tri.lineTo (cx + 4.4f, cy);
                tri.lineTo (cx - 2.6f, cy + 4.6f);
                tri.closeSubPath();
                fill (tri);
            }
            else if (id == Record)
            {
                juce::Path dot;
                dot.addEllipse (cx - 3.4f, cy - 3.4f, 6.8f, 6.8f);
                fill (dot);
            }
            else if (id == Bass)
            {
                // 琴身：单枚修长圆（与吉他的 8 字形区分）
                juce::Path body;
                body.addEllipse (cx - 3.0f, cy + 0.2f, 6.0f, 6.2f);
                fill (body);
                // 长琴颈直通琴头
                juce::Path neck;
                neck.addRectangle (cx - 0.7f, cy - 6.6f, 1.4f, 7.2f);
                fill (neck);
                // 琴头两侧弦钮（贝斯标志性的长颈 + 弦钮）
                juce::Path pegs;
                pegs.addRectangle (cx - 2.4f, cy - 6.2f, 1.5f, 1.0f);
                pegs.addRectangle (cx + 0.9f, cy - 5.0f, 1.5f, 1.0f);
                fill (pegs);
            }
            else if (id == Synth)
            {
                // 合成器面板外框（圆角）
                juce::Path frame;
                frame.addRoundedRectangle (cx - 6.0f, cy - 4.5f, 12.0f, 9.0f, 1.5f);
                stroke (frame);
                // 锯齿波形（放大振幅铺满面板，避免小尺寸下只剩一条斜线感）
                juce::Path wave;
                wave.startNewSubPath (cx - 4.2f, cy + 1.8f);
                wave.lineTo (cx - 1.6f, cy - 2.2f);
                wave.lineTo (cx + 0.6f, cy + 1.8f);
                wave.lineTo (cx + 2.4f, cy - 1.2f);
                wave.lineTo (cx + 4.2f, cy + 1.8f);
                stroke (wave);
            }
            else if (id == Speaker)
            {
                // 箱体
                juce::Path box;
                box.addRoundedRectangle (cx - 4.5f, cy - 5.5f, 9.0f, 11.0f, 1.5f);
                stroke (box);
                // 低音振膜
                juce::Path cone;
                cone.addEllipse (cx - 2.6f, cy - 0.8f, 5.2f, 5.2f);
                stroke (cone);
                // 高音单元
                juce::Path tweeter;
                tweeter.addEllipse (cx - 1.3f, cy - 4.4f, 2.6f, 2.0f);
                fill (tweeter);
            }
            else if (id == System)
            {
                // 系统：显示器（屏幕外框 + 支架 + 底座）
                juce::Path screen;
                screen.addRoundedRectangle (cx - 6.0f, cy - 5.5f, 12.0f, 9.0f, 1.5f);
                stroke (screen);
                juce::Path stand;
                stand.startNewSubPath (cx, cy + 3.5f);
                stand.lineTo (cx, cy + 5.4f);
                stand.startNewSubPath (cx - 3.2f, cy + 5.6f);
                stand.lineTo (cx + 3.2f, cy + 5.6f);
                stroke (stand);
            }
            else if (id == Out)
            {
                // 输出：开口方箱 + 向右穿出的箭头
                juce::Path box;
                box.startNewSubPath (cx + 2.0f, cy - 4.5f);
                box.lineTo (cx - 4.5f, cy - 4.5f);
                box.lineTo (cx - 4.5f, cy + 4.5f);
                box.lineTo (cx + 2.0f, cy + 4.5f);
                stroke (box);
                juce::Path arrow;
                arrow.startNewSubPath (cx - 1.5f, cy);
                arrow.lineTo (cx + 5.8f, cy);
                arrow.startNewSubPath (cx + 3.4f, cy - 2.2f);
                arrow.lineTo (cx + 5.8f, cy);
                arrow.startNewSubPath (cx + 3.4f, cy + 2.2f);
                arrow.lineTo (cx + 5.8f, cy);
                stroke (arrow);
            }
            else if (id == Bus)
            {
                // 总线：三路汇入节点再输出
                juce::Path in;
                in.startNewSubPath (cx - 5.8f, cy - 3.2f);
                in.lineTo (cx - 1.4f, cy);
                in.startNewSubPath (cx - 5.8f, cy + 3.2f);
                in.lineTo (cx - 1.4f, cy);
                in.startNewSubPath (cx - 5.8f, cy);
                in.lineTo (cx - 1.4f, cy);
                stroke (in);
                juce::Path node;
                node.addEllipse (cx - 2.2f, cy - 2.2f, 4.4f, 4.4f);
                fill (node);
                juce::Path out;
                out.startNewSubPath (cx + 2.2f, cy);
                out.lineTo (cx + 5.8f, cy);
                stroke (out);
            }
            else if (id == Pedal)
            {
                // 效果踏板：外框 + 顶部两旋钮 + 中央脚踏开关
                juce::Path body;
                body.addRoundedRectangle (cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, 2.0f);
                stroke (body);
                juce::Path knobs;
                knobs.addEllipse (cx - 3.4f, cy - 3.4f, 1.8f, 1.8f);
                knobs.addEllipse (cx + 1.6f, cy - 3.4f, 1.8f, 1.8f);
                fill (knobs);
                juce::Path foot;
                foot.addEllipse (cx - 1.9f, cy + 0.2f, 3.8f, 3.8f);
                stroke (foot);
            }
            else if (id == Bell)
            {
                // 铃铛：钟形穹顶 + 下沿横杆 + 铃舌
                juce::Path dome;
                dome.startNewSubPath (cx - 4.2f, cy + 2.4f);
                dome.quadraticTo (cx - 4.2f, cy - 4.8f, cx, cy - 4.8f);
                dome.quadraticTo (cx + 4.2f, cy - 4.8f, cx + 4.2f, cy + 2.4f);
                dome.closeSubPath();
                stroke (dome);
                juce::Path rim;
                rim.addRectangle (cx - 5.4f, cy + 2.4f, 10.8f, 1.6f);
                fill (rim);
                juce::Path clapper;
                clapper.addEllipse (cx - 1.2f, cy + 4.6f, 2.4f, 2.0f);
                fill (clapper);
            }
            else if (id == Fx)
            {
                // 四角闪光星（凹边菱形，比十字加号更有「效果」感）
                juce::Path s;
                s.startNewSubPath (cx, cy - 5.8f);
                s.quadraticTo (cx, cy, cx + 5.8f, cy);
                s.quadraticTo (cx, cy, cx, cy + 5.8f);
                s.quadraticTo (cx, cy, cx - 5.8f, cy);
                s.quadraticTo (cx, cy, cx, cy - 5.8f);
                s.closeSubPath();
                fill (s);
            }
        }
    }
}
