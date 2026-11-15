#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "I18n.h"

//==============================================================================
// 本机音频驱动 / 端口目录
//
// 每个驱动条目提供两个输入端口（L / R）与两个输出端口（L / R）。
// 端口是「路由地址」，不是设备本身：主设备端口在音频回调里直接读写，
// 其它驱动端口由 AudioEngine 按需打开为辅助设备，经无锁环形缓冲收发。
//
// 不做全量 createDevice() 探测通道名 —— 那会初始化所有 ASIO/WASAPI 端点，
// 既慢又可能与已打开的主设备抢占驱动。实际通道数在设备打开时按真实值映射。
//==============================================================================
namespace mb
{
    struct DeviceEntry
    {
        int typeIndex = -1;
        juce::String typeName;      // "ASIO" / "Windows Audio" / "DirectSound"
        juce::String inputName;     // 该条目实际提供的输入名，可为空
        juce::String outputName;    // 该条目实际提供的输出名，可为空
        juce::String createInputName;   // 调 createDevice 时保证两侧都是合法名字
        juce::String createOutputName;
        juce::String displayName;   // 界面短名
        juce::String typeShortName; // 界面短驱动标签：WASAPI / WASAPI·Exclusive / ASIO ...
        bool isAsio = false;
        bool isMaster = false;
        bool isLoopback = false;    // 虚拟设备：系统声音回环捕获（WASAPI loopback）
        bool available = true;      // 辅助设备打开失败时置 false
        juce::String error;

        // 真实通道（探测成功才有值）。默认 2 是旧版固定 L/R 语义，
        // 端口编号必须与旧版一致，额外通道一律「成对追加」到列表末尾。
        int inputChannels = 2;
        int outputChannels = 2;
        juce::StringArray inputChannelNames, outputChannelNames;
    };

    struct PortInfo
    {
        int deviceIndex = -1;
        int channelIndex = 0;       // 0 = L / 1 = R；额外通道为真实物理通道号
        bool input = true;
        juce::String name;          // "设备名 · L" 或 "设备名 · Input 3"
        juce::String deviceName;
        juce::String typeName;
        juce::String channelName;   // 真实通道名（可为空）
        juce::String deviceLabel;   // 显示用：备注名在左、设备名在右（分组标题 / 菜单）
    };

    struct DeviceChoice
    {
        juce::String typeName;
        juce::String inputName;
        juce::String outputName;
        double sampleRate = 0.0;    // 0 = 自动
        int bufferSize = 0;         // 0 = 自动
        bool valid = false;
    };

    /** 设备选择持久化（便携化：exe 同级 MicroBridgeData/device.xml）。 */
    class DevicePreferences
    {
    public:
        static juce::File file()
        {
            return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                    .getParentDirectory().getChildFile ("MicroBridgeData")
                    .getChildFile ("device.xml");
        }

        DeviceChoice load() const
        {
            DeviceChoice c;
            auto xml = juce::parseXML (file());
            if (xml == nullptr || ! xml->hasTagName ("MicroBridgeDevice")) return c;

            c.typeName    = xml->getStringAttribute ("type");
            c.inputName   = xml->getStringAttribute ("input");
            c.outputName  = xml->getStringAttribute ("output");
            c.sampleRate  = xml->getDoubleAttribute ("rate", 0.0);
            c.bufferSize  = xml->getIntAttribute    ("buffer", 0);
            c.valid       = c.typeName.isNotEmpty();
            return c;
        }

        void save (const DeviceChoice& c) const
        {
            juce::XmlElement xml ("MicroBridgeDevice");
            xml.setAttribute ("type",   c.typeName);
            xml.setAttribute ("input",  c.inputName);
            xml.setAttribute ("output", c.outputName);
            xml.setAttribute ("rate",   c.sampleRate);
            xml.setAttribute ("buffer", c.bufferSize);
            // 保留既有名单：save 会整文件重写，不搬过去的话
            // 设置辅助设备 / 取消主通道勾选后再改默认设备就会把名单冲掉。
            if (auto old = juce::parseXML (file()))
            {
                if (auto* h = old->getChildByName ("auxShown"))
                    xml.addChildElement (new juce::XmlElement (*h));
                if (auto* h = old->getChildByName ("masterOff"))
                    xml.addChildElement (new juce::XmlElement (*h));
                if (auto* h = old->getChildByName ("deviceNotes"))
                    xml.addChildElement (new juce::XmlElement (*h));
            }
            file().getParentDirectory().createDirectory();
            xml.writeTo (file());
        }

        /** 辅助设备显示名单（按 驱动类型|完整设备名 逐条记录；只记录非主驱动）。
            语义 = 「勾选了才出现在通道菜单」：默认仅主驱动可见，用户在设备设置页
            勾选某台设备后它才列入。旧版 auxHidden（隐藏名单=默认全可见）在
            DeviceCatalog::reloadAuxPrefs 里自动迁移。 */
        struct ShownAux { juce::StringArray inputs, outputs; };

        ShownAux loadShown() const
        {
            ShownAux s;
            auto xml = juce::parseXML (file());
            if (xml == nullptr) return s;
            if (auto* e = xml->getChildByName ("auxShown"))
                for (auto* c : e->getChildIterator())
                {
                    if (c->hasTagName ("in"))  s.inputs.add  (c->getStringAttribute ("name"));
                    if (c->hasTagName ("out")) s.outputs.add (c->getStringAttribute ("name"));
                }
            return s;
        }

        void saveShown (const ShownAux& s) const
        {
            auto xml = juce::parseXML (file());
            if (xml == nullptr)
                xml = std::make_unique<juce::XmlElement> ("MicroBridgeDevice");
            while (auto* old = xml->getChildByName ("auxShown"))
                xml->removeChildElement (old, true);
            while (auto* old = xml->getChildByName ("auxHidden"))
                xml->removeChildElement (old, true);       // 新名单写入后旧语义作废
            auto* e = new juce::XmlElement ("auxShown");
            // v2 = 「其它驱动设备默认都不勾选」规则下写入的名单；
            // 没有 version（或 version<2）的旧名单会在启动时一次性作废。
            e->setAttribute ("version", 2);
            for (const auto& n : s.inputs)  e->createNewChildElement ("in") ->setAttribute ("name", n);
            for (const auto& n : s.outputs) e->createNewChildElement ("out")->setAttribute ("name", n);
            xml->addChildElement (e);
            file().getParentDirectory().createDirectory();
            xml->writeTo (file());
        }

        /** 主设备通道的「取消勾选」名单：默认全部可见（界面上全部勾选），
            只有被用户取消勾选的通道对才记录在这里。键 = auxKey|@通道对基址。 */
        struct MasterOff { juce::StringArray inputs, outputs; };

        MasterOff loadMasterOff() const
        {
            MasterOff h;
            auto xml = juce::parseXML (file());
            if (xml == nullptr) return h;
            if (auto* e = xml->getChildByName ("masterOff"))
                for (auto* c : e->getChildIterator())
                {
                    if (c->hasTagName ("in"))  h.inputs.add  (c->getStringAttribute ("name"));
                    if (c->hasTagName ("out")) h.outputs.add (c->getStringAttribute ("name"));
                }
            return h;
        }

        void saveMasterOff (const MasterOff& h) const
        {
            auto xml = juce::parseXML (file());
            if (xml == nullptr)
                xml = std::make_unique<juce::XmlElement> ("MicroBridgeDevice");
            while (auto* old = xml->getChildByName ("masterOff"))
                xml->removeChildElement (old, true);
            auto* e = new juce::XmlElement ("masterOff");
            for (const auto& n : h.inputs)  e->createNewChildElement ("in") ->setAttribute ("name", n);
            for (const auto& n : h.outputs) e->createNewChildElement ("out")->setAttribute ("name", n);
            xml->addChildElement (e);
            file().getParentDirectory().createDirectory();
            xml->writeTo (file());
        }

        //======================================================================
        /** 设备备注表：key = 驱动类型|输入名|输出名（同一物理设备只存一条），
            value = 用户填写的备注名。写入时读-改-写整份 XML，不会冲掉其它名单。 */
        using DeviceNotes = std::vector<std::pair<juce::String, juce::String>>;

        DeviceNotes loadNotes() const
        {
            DeviceNotes result;
            if (auto xml = juce::parseXML (file()))
                if (auto* e = xml->getChildByName ("deviceNotes"))
                    for (auto* c : e->getChildIterator())
                        if (c->hasTagName ("note"))
                        {
                            const auto key = c->getStringAttribute ("key");
                            if (key.isNotEmpty())
                                result.emplace_back (key, c->getStringAttribute ("text"));
                        }
            return result;
        }

        void saveNotes (const DeviceNotes& notes) const
        {
            auto xml = juce::parseXML (file());
            if (xml == nullptr)
                xml = std::make_unique<juce::XmlElement> ("MicroBridgeDevice");
            while (auto* old = xml->getChildByName ("deviceNotes"))
                xml->removeChildElement (old, true);
            auto* e = new juce::XmlElement ("deviceNotes");
            for (const auto& n : notes)
            {
                if (n.first.isEmpty() || n.second.isEmpty()) continue;
                auto* c = e->createNewChildElement ("note");
                c->setAttribute ("key",  n.first);
                c->setAttribute ("text", n.second);
            }
            xml->addChildElement (e);
            file().getParentDirectory().createDirectory();
            xml->writeTo (file());
        }
    };

    class DeviceCatalog
    {
    public:
        void scan (juce::AudioDeviceManager& dm)
        {
            devices_.clear();
            inputPorts_.clear();
            outputPorts_.clear();
            typeNames_.clear();

            const auto& types = dm.getAvailableDeviceTypes();
            for (int t = 0; t < types.size(); ++t)
            {
                auto* type = types[t];
                if (type == nullptr) continue;

                type->scanForDevices();
                const auto typeName = type->getTypeName();
                const auto inNames  = type->getDeviceNames (true);
                const auto outNames = type->getDeviceNames (false);
                if (inNames.isEmpty() && outNames.isEmpty()) continue;

                typeNames_.addIfNotAlreadyThere (typeName);

                // 同一驱动的输入名/输出名常常不同（WASAPI/DirectSound），按名字归并。
                juce::StringArray names;
                for (auto& n : inNames)  names.addIfNotAlreadyThere (n);
                for (auto& n : outNames) names.addIfNotAlreadyThere (n);

                for (auto& n : names)
                {
                    DeviceEntry e;
                    e.typeIndex  = t;
                    e.typeName   = typeName;
                    e.inputName  = inNames.contains (n)  ? n : juce::String();
                    e.outputName = outNames.contains (n) ? n : juce::String();
                    // JUCE 的 WASAPI createDevice 会用 indexOf 查两个名字，
                    // 传空串会索引 -1；因此缺失的一侧补同类驱动的首个合法名字。
                    e.createInputName  = e.inputName.isNotEmpty()  ? e.inputName
                                        : (inNames.isEmpty()  ? juce::String() : inNames[0]);
                    e.createOutputName = e.outputName.isNotEmpty() ? e.outputName
                                        : (outNames.isEmpty() ? juce::String() : outNames[0]);
                    e.displayName     = shortName (n);
                    e.typeShortName   = shortTypeName (typeName);
                    e.isAsio          = typeName.containsIgnoreCase ("ASIO");
                    // ASIO 驱动数量少、且多通道是刚需，构造期即可探测真实通道名。
                    // 其它驱动（WASAPI/DirectSound）不在此处全量 createDevice：
                    // 那会初始化所有端点，既慢又可能抢占已打开的主设备。
                    if (e.isAsio)
                        probeChannels (type, e);
                    devices_.push_back (e);
                }
            }

            // 虚拟设备固定排末尾（端口 id 依赖设备枚举顺序，见 rebuildPorts）：
            // 系统声音回环捕获，输入专用 2 路（WASAPI loopback，见 LoopbackDevice.h）
            {
                DeviceEntry e;
                e.typeIndex      = -1;                      // 无对应驱动类型
                e.typeName       = S("System Loopback");    // 独立类型名，auxKey 天然隔离
                e.inputName      = tr ("系统声音（回环）", "System Sound (Loopback)");
                e.displayName    = e.inputName;
                e.typeShortName  = S("WASAPI");
                e.inputChannels  = 2;
                e.isLoopback     = true;
                devices_.push_back (e);
            }

            reloadNotes();      // 备注必须先于端口显示标签计算
            rebuildPorts();
            reloadAuxPrefs();
        }

        /** 用已打开的主设备回写真实通道名/数量，然后重编端口。
            主设备走的是 AudioDeviceManager 的回调，只有显式启用全部通道
            才能拿到第 3 路以后的采样（见 AudioEngine::applySetup）。 */
        void setMasterChannels (const juce::String& typeName,
                                const juce::String& inputName,
                                const juce::String& outputName,
                                const juce::StringArray& inNames,
                                const juce::StringArray& outNames)
        {
            bool changed = false;
            for (auto& d : devices_)
            {
                if (! d.isMaster || d.typeName != typeName) continue;
                if (inputName.isNotEmpty() && d.inputName == inputName && ! inNames.isEmpty())
                {
                    d.inputChannelNames = inNames;
                    d.inputChannels     = inNames.size();
                    changed = true;
                }
                if (outputName.isNotEmpty() && d.outputName == outputName && ! outNames.isEmpty())
                {
                    d.outputChannelNames = outNames;
                    d.outputChannels     = outNames.size();
                    changed = true;
                }
            }
            if (changed) rebuildPorts();
        }

        /** ASIO 驱动真实通道探测：createDevice 只构造对象、不 open，不会与主设备
            抢占音频流。结果按驱动缓存，避免每次 scan 都重新加载 ASIO 驱动 DLL。 */
        void probeChannels (juce::AudioIODeviceType* type, DeviceEntry& e)
        {
            if (type == nullptr) return;
            const auto key = auxKey (e.typeName, e.inputName + S("|") + e.outputName);
            const auto hit = probeCache_.find (key);
            if (hit != probeCache_.end())
            {
                const auto& p = hit->second;
                e.inputChannels  = p.inputChannels;   e.outputChannels  = p.outputChannels;
                e.inputChannelNames  = p.inputChannelNames;
                e.outputChannelNames = p.outputChannelNames;
                return;
            }

            DeviceEntry p;
            std::unique_ptr<juce::AudioIODevice> probe (
                type->createDevice (e.createOutputName, e.createInputName));
            if (probe != nullptr)
            {
                p.inputChannelNames  = probe->getInputChannelNames();
                p.outputChannelNames = probe->getOutputChannelNames();
                p.inputChannels  = juce::jmax (2, p.inputChannelNames.size());
                p.outputChannels = juce::jmax (2, p.outputChannelNames.size());
            }
            probeCache_[key] = p;
            e.inputChannels  = p.inputChannels;   e.outputChannels  = p.outputChannels;
            e.inputChannelNames  = p.inputChannelNames;
            e.outputChannelNames = p.outputChannelNames;
        }

        /** 惰性探测指定驱动类型下非 ASIO 设备的真实通道数/名。
            createDevice 只构造设备对象、不 open，不会抢占已打开的音频流；
            结果经 probeCache_ 缓存，重复调用零开销。Setup 对话框打开 /
            换驱动 / 手动「刷新」时调用，避免启动时全量初始化所有端点的开销。
            返回是否有设备的通道数发生变化（变化即已重编端口）。 */
        bool probeTypeChannels (juce::AudioDeviceManager& dm, const juce::String& typeName)
        {
            bool changed = false;
            for (auto& d : devices_)
            {
                if (d.typeName != typeName || d.isAsio || d.typeIndex < 0) continue;
                auto* type = typeFor (dm, d.typeIndex);
                if (type == nullptr) continue;
                const int inBefore  = d.inputChannels;
                const int outBefore = d.outputChannels;
                probeChannels (type, d);
                changed |= d.inputChannels != inBefore || d.outputChannels != outBefore;
            }
            if (changed) rebuildPorts();
            return changed;
        }

        void markMaster (const juce::String& typeName,
                         const juce::String& inputName,
                         const juce::String& outputName)
        {
            for (auto& d : devices_)
            {
                d.isMaster = d.typeName == typeName
                          && ((d.inputName.isNotEmpty()  && d.inputName  == inputName)
                           || (d.outputName.isNotEmpty() && d.outputName == outputName));
                if (d.isMaster)
                {
                    d.available = true;
                    d.error = {};
                }
            }
        }

        void rebuildPorts()
        {
            inputPorts_.clear();
            outputPorts_.clear();

            // 第一遍：每台设备固定的 L / R，编号与旧版完全一致。
            // 旧 session.json / 预设里存的端口 id 必须继续指向原来的设备。
            for (size_t i = 0; i < devices_.size(); ++i)
            {
                const auto& d = devices_[i];
                const auto labelIn  = deviceLabelFor (d, true);
                const auto labelOut = deviceLabelFor (d, false);
                for (int c = 0; c < 2; ++c)
                {
                    const auto suffix = c == 0 ? S(" · L") : S(" · R");
                    if (d.inputName.isNotEmpty())
                        inputPorts_.push_back ({ static_cast<int> (i), c, true,
                                                d.displayName + suffix, d.displayName,
                                                d.typeName, {}, labelIn });
                    if (d.outputName.isNotEmpty())
                        outputPorts_.push_back ({ static_cast<int> (i), c, false,
                                                 d.displayName + suffix, d.displayName,
                                                 d.typeName, {}, labelOut });
                }
            }

            // 第二遍：多通道设备把第 3 路起按真实通道名成对追加到末尾。
            // 「成对 + 同设备相邻」保证既有 p%2==0 / p+1 立体声配对逻辑仍成立。
            for (size_t i = 0; i < devices_.size(); ++i)
            {
                const auto& d = devices_[i];
                if (d.inputName.isNotEmpty()  && d.inputChannels  > 2)
                    appendExtraPorts (inputPorts_,  d, static_cast<int> (i), true,
                                      deviceLabelFor (d, true));
                if (d.outputName.isNotEmpty() && d.outputChannels > 2)
                    appendExtraPorts (outputPorts_, d, static_cast<int> (i), false,
                                      deviceLabelFor (d, false));
            }
        }

        int deviceCount()    const noexcept { return static_cast<int> (devices_.size()); }
        int inputPortCount() const noexcept { return static_cast<int> (inputPorts_.size()); }
        int outputPortCount()const noexcept { return static_cast<int> (outputPorts_.size()); }

        const std::vector<DeviceEntry>& devices()     const noexcept { return devices_; }
        const std::vector<PortInfo>&    inputPorts()  const noexcept { return inputPorts_; }
        const std::vector<PortInfo>&    outputPorts() const noexcept { return outputPorts_; }
        const juce::StringArray&        typeNames()   const noexcept { return typeNames_; }

        juce::AudioIODeviceType* typeFor (juce::AudioDeviceManager& dm, int typeIndex) const
        {
            const auto& types = dm.getAvailableDeviceTypes();
            return typeIndex >= 0 && typeIndex < types.size() ? types[typeIndex] : nullptr;
        }

        //======================================================================
        // 设备备注：所有设备（主设备 / 其它设备、输入 / 输出端）共用一条备注，
        // 按「驱动类型|输入名|输出名」定位，重启后从 device.xml 恢复。
        //======================================================================

        /** 备注键：同一物理设备条目（可能输入名与输出名不同）只存一条。 */
        static juce::String noteKey (const DeviceEntry& d)
        {
            return d.typeName + S("|") + d.inputName + S("|") + d.outputName;
        }

        /** 用界面上的任一设备名反查备注键。 */
        juce::String noteKeyFor (const juce::String& typeName, const juce::String& name) const
        {
            for (const auto& d : devices_)
                if (d.typeName == typeName && (d.inputName == name || d.outputName == name))
                    return noteKey (d);
            return typeName + S("|") + name + S("|") + name;
        }

        juce::String noteFor (const juce::String& typeName, const juce::String& name) const
        {
            return noteForKey (noteKeyFor (typeName, name));
        }

        juce::String noteForKey (const juce::String& key) const
        {
            const auto it = notes_.find (key);
            return it == notes_.end() ? juce::String() : it->second;
        }

        //======================================================================
        // 通道对备注：ASIO 等多通道设备的输入/输出通道（按立体声对，L/R 一组）
        // 也可单独命名。与设备备注共用同一张表 / 同一个 device.xml 节点，
        // 键 = 设备备注键 + "|@" + 通道对基址 + "|in|out"（方向分开防串）。
        //======================================================================

        static juce::String channelNoteKey (const DeviceEntry& d, bool input, int pairBase)
        {
            return noteKey (d) + S("|@") + juce::String (pairBase)
                 + S("|") + (input ? S("in") : S("out"));
        }

        juce::String channelNote (const DeviceEntry& d, bool input, int pairBase) const
        {
            return noteForKey (channelNoteKey (d, input, pairBase));
        }

        /** 按端口取所属通道对的备注（无备注返回空串）。 */
        juce::String channelNoteForPort (const PortInfo& p) const
        {
            if (p.deviceIndex < 0 || p.deviceIndex >= static_cast<int> (devices_.size()))
                return {};
            const auto& d = devices_[static_cast<size_t> (p.deviceIndex)];
            const int pair = p.channelIndex - (p.channelIndex % 2);
            return noteForKey (channelNoteKey (d, p.input, pair));
        }

        /** 设置 / 清空备注：立即写盘并刷新所有端口的显示标签。空串 = 删除备注。 */
        void setNote (const juce::String& key, const juce::String& text)
        {
            const auto clean = text.trim();
            if (clean.isEmpty()) notes_.erase (key);
            else                 notes_[key] = clean;

            DevicePreferences::DeviceNotes out;
            for (const auto& n : notes_) out.emplace_back (n.first, n.second);
            DevicePreferences{}.saveNotes (out);
            rebuildPorts();
        }

        /** 「备注名 (设备名)」的完整显示串（下拉 / 菜单 / 提示用）。 */
        juce::String labelledName (const juce::String& typeName, const juce::String& name) const
        {
            const auto n = noteFor (typeName, name);
            return n.isEmpty() ? name : n + S(" (") + name + S(")");
        }

        /** 端口标签：通道对备注优先、其次设备备注，
            格式「备注名 (原名)」，通道条 / 悬停提示同样可辨认。 */
        juce::String portLabel (bool input, int portId) const
        {
            const auto& list = input ? inputPorts_ : outputPorts_;
            if (portId < 0 || portId >= static_cast<int> (list.size()))
                return (input ? S("IN") : S("OUT")) + juce::String (portId + 1);
            const auto& p = list[static_cast<size_t> (portId)];
            const auto n = noteForPort (p);
            return n.isEmpty() ? p.name : n + S(" (") + p.name + S(")");
        }

        int portDeviceIndex (bool input, int portId) const
        {
            const auto& list = input ? inputPorts_ : outputPorts_;
            if (portId < 0 || portId >= static_cast<int> (list.size())) return -1;
            return list[static_cast<size_t> (portId)].deviceIndex;
        }

        int deviceIndexOfType (const juce::String& typeName, const juce::String& name) const
        {
            for (size_t i = 0; i < devices_.size(); ++i)
                if (devices_[i].typeName == typeName
                    && (devices_[i].inputName == name || devices_[i].outputName == name))
                    return static_cast<int> (i);
            return -1;
        }

        juce::StringArray deviceNamesForType (const juce::String& typeName, bool wantInput) const
        {
            juce::StringArray result;
            for (const auto& d : devices_)
                if (d.typeName == typeName)
                {
                    const auto& n = wantInput ? d.inputName : d.outputName;
                    if (n.isNotEmpty()) result.addIfNotAlreadyThere (n);
                }
            return result;
        }

        //======================================================================
        // 端点配对：Windows 把同一块声卡的采集端与播放端报成两个不同名字
        // （例：B1 Output (VB-CABLE 1) / A1 Input (VB-CABLE 1)、
        //       麦克风 (Wuying ASP Audio) / 扬声器 (Wuying ASP Audio)）。
        // 按括号里的物理设备名把两端配对，选中任意一侧都能得到完整双向主设备。
        // 注意：底层 devices_ 条目保持原样、顺序不变 —— rebuildPorts() 的
        // 端口 id 依赖它，旧 session.json / 预设的路由才不会错位。
        //======================================================================

        /** 端点名 -> 物理设备键；认不出来的名字返回空（不配对，避免误绑）。 */
        static juce::String containerKey (const juce::String& endpointName)
        {
            const auto s = endpointName.trim();
            const int open  = s.lastIndexOfChar ('(');
            const int close = s.lastIndexOfChar (')');
            if (open >= 0 && close > open + 1)
                return s.substring (open + 1, close).trim().toLowerCase();

            // DirectSound 的默认别名没有括号，单独归一到同一容器。
            const auto t = s.toLowerCase();
            if (t.contains (S("主声音")) || t.contains (S("primary sound")))
                return S("primary");
            return {};
        }

        /** 端点名里的序号（B3 / A3 / CABLE 3 -> 3），无序号返回 -1。 */
        static int endpointOrdinal (const juce::String& n)
        {
            for (int i = 0; i < n.length(); ++i)
            {
                if (! juce::CharacterFunctions::isDigit (n[i])) continue;
                int v = 0;
                while (i < n.length() && juce::CharacterFunctions::isDigit (n[i]))
                {
                    v = v * 10 + static_cast<int> (n[i] - '0');
                    ++i;
                }
                return v;
            }
            return -1;
        }

        /** 同一驱动下与给定输入端点配对的输出端点名，找不到返回空。 */
        juce::String pairedOutputName (const juce::String& typeName,
                                       const juce::String& inputName) const
        {
            const auto key = containerKey (inputName);
            if (key.isEmpty() || inputName.isEmpty()) return {};
            juce::StringArray outs;
            for (const auto& d : devices_)
                if (d.typeName == typeName && d.outputName.isNotEmpty()
                    && containerKey (d.outputName) == key)
                    outs.addIfNotAlreadyThere (d.outputName);
            if (outs.isEmpty()) return {};
            const int ord = endpointOrdinal (inputName);
            if (ord >= 0)
                for (const auto& o : outs)
                    if (endpointOrdinal (o) == ord) return o;
            return outs[0];
        }

        /** 对称版本：与给定输出端点配对的输入端点名。 */
        juce::String pairedInputName (const juce::String& typeName,
                                      const juce::String& outputName) const
        {
            const auto key = containerKey (outputName);
            if (key.isEmpty() || outputName.isEmpty()) return {};
            juce::StringArray ins;
            for (const auto& d : devices_)
                if (d.typeName == typeName && d.inputName.isNotEmpty()
                    && containerKey (d.inputName) == key)
                    ins.addIfNotAlreadyThere (d.inputName);
            if (ins.isEmpty()) return {};
            const int ord = endpointOrdinal (outputName);
            if (ord >= 0)
                for (const auto& n : ins)
                    if (endpointOrdinal (n) == ord) return n;
            return ins[0];
        }

        const DeviceEntry* entryWithInput (const juce::String& typeName,
                                           const juce::String& name) const
        {
            if (name.isEmpty()) return nullptr;
            for (const auto& d : devices_)
                if (d.typeName == typeName && d.inputName == name) return &d;
            return nullptr;
        }

        const DeviceEntry* entryWithOutput (const juce::String& typeName,
                                            const juce::String& name) const
        {
            if (name.isEmpty()) return nullptr;
            for (const auto& d : devices_)
                if (d.typeName == typeName && d.outputName == name) return &d;
            return nullptr;
        }

        /** 把设备下拉里的名字解析成真正交给 AudioDeviceManager 的两侧名字。
            双向条目原样返回；单向端点用配对端点补齐另一侧。 */
        DeviceChoice resolveChoice (const juce::String& typeName,
                                    const juce::String& name) const
        {
            DeviceChoice c;
            c.typeName = typeName;
            c.valid = true;
            if (name.isEmpty()) return c;

            const DeviceEntry* e = nullptr;
            for (const auto& d : devices_)
                if (d.typeName == typeName
                    && (d.inputName == name || d.outputName == name)) { e = &d; break; }

            if (e == nullptr)       // 旧配置残留 / 设备已拔掉：沿用旧的回退规则
            {
                const auto ins  = deviceNamesForType (typeName, true);
                const auto outs = deviceNamesForType (typeName, false);
                c.inputName  = ins.contains (name)  ? name : (ins.isEmpty()  ? juce::String() : ins[0]);
                c.outputName = outs.contains (name) ? name : (outs.isEmpty() ? juce::String() : outs[0]);
                return c;
            }

            if (e->inputName.isNotEmpty() && e->outputName.isNotEmpty())
            {
                c.inputName  = e->inputName;
                c.outputName = e->outputName;
            }
            else if (e->inputName.isNotEmpty())
            {
                c.inputName  = e->inputName;
                c.outputName = pairedOutputName (typeName, e->inputName);
                if (c.outputName.isEmpty())
                {
                    const auto outs = deviceNamesForType (typeName, false);
                    if (! outs.isEmpty()) c.outputName = outs[0];
                }
            }
            else
            {
                c.outputName = e->outputName;
                c.inputName  = pairedInputName (typeName, e->outputName);
                if (c.inputName.isEmpty())
                {
                    const auto ins = deviceNamesForType (typeName, true);
                    if (! ins.isEmpty()) c.inputName = ins[0];
                }
            }
            return c;
        }

        //======================================================================
        // 辅助设备可见性：设备设置页里勾选的输入/输出设备才会列到通道菜单。
        // 端口编号 = inputPorts()/outputPorts() 的下标，恒定不变；
        // 隐藏只影响「新连接的可选项」，已建立的连接照常工作（音频侧不看可见性）。
        // 语义 = 显示名单：主驱动端口永远可见，其它设备勾选了才可见。
        //======================================================================

        /** 键 = 驱动类型 | 完整设备名（WASAPI 与 DirectSound 可能同名，带上类型防误伤）。 */
        static juce::String auxKey (const juce::String& typeName, const juce::String& fullName)
        {
            return typeName + S("|") + fullName;
        }

        /** 该端口是否出现在通道的输入/输出设备菜单里。
            主设备：默认全部可见（设备设置页里全部勾选），被取消勾选的通道对才隐藏。
            其它设备：勾选了才列入（显示名单语义）。 */
        bool portVisible (bool input, int portId) const
        {
            const auto& list = input ? inputPorts_ : outputPorts_;
            if (portId < 0 || portId >= static_cast<int> (list.size())) return false;
            const auto& p = list[static_cast<size_t> (portId)];
            const auto& d = devices_[static_cast<size_t> (p.deviceIndex)];
            const auto& n = input ? d.inputName : d.outputName;
            if (n.isEmpty()) return false;
            const auto key = auxKey (d.typeName, n);

            if (d.isMaster)
            {
                const int pair = p.channelIndex - (p.channelIndex % 2);
                const auto& off = input ? masterOffIn_ : masterOffOut_;
                return ! off.contains (key + S("|@") + juce::String (pair))
                    && ! off.contains (key);
            }
            return (input ? shownIn_ : shownOut_).contains (key);
        }

        /** 设备设置页调用：写入主设备被取消勾选的通道对。 */
        void setMasterOff (juce::StringArray inKeys, juce::StringArray outKeys)
        {
            masterOffIn_  = std::move (inKeys);
            masterOffOut_ = std::move (outKeys);
        }

        juce::StringArray masterOffKeys (bool input) const
        {
            return input ? masterOffIn_ : masterOffOut_;
        }

        /** 设备设置页调用：写入显示名单（键 = auxKey 产物）。 */
        void setAuxShown (juce::StringArray inKeys, juce::StringArray outKeys)
        {
            shownIn_  = std::move (inKeys);
            shownOut_ = std::move (outKeys);
        }

        juce::StringArray auxShownKeys (bool input) const
        {
            return input ? shownIn_ : shownOut_;
        }

    private:
        void reloadNotes()
        {
            notes_.clear();
            for (const auto& n : DevicePreferences{}.loadNotes())
                notes_[n.first] = n.second;
        }

        /** 端口备注：通道对备注优先，其次设备备注。 */
        juce::String noteForPort (const PortInfo& p) const
        {
            if (p.deviceIndex < 0 || p.deviceIndex >= static_cast<int> (devices_.size()))
                return {};
            const auto& d = devices_[static_cast<size_t> (p.deviceIndex)];
            const int pair = p.channelIndex - (p.channelIndex % 2);
            auto n = noteForKey (channelNoteKey (d, p.input, pair));
            return n.isNotEmpty() ? n : noteForKey (noteKey (d));
        }

        /** 端口分组标题用的显示串：备注名 (设备短名)。 */
        juce::String deviceLabelFor (const DeviceEntry& d, bool /*input*/) const
        {
            const auto n = noteForKey (noteKey (d));
            return n.isEmpty() ? d.displayName : n + S(" (") + d.displayName + S(")");
        }

        static juce::String shortName (const juce::String& n)
        {
            // WASAPI 设备名常带一长串厂商后缀，界面里截短但保留完整名在 tooltip。
            if (n.length() > 26) return n.substring (0, 24) + S("…");
            return n;
        }

        /** 驱动类型短标签：设备设置页里同一物理设备会被多种驱动重复列出，
            必须把类型显式标出来才能分辨。 */
        static juce::String shortTypeName (const juce::String& t)
        {
            if (t.containsIgnoreCase ("ASIO"))             return S("ASIO");
            if (t.containsIgnoreCase ("Low Latency"))      return S("WASAPI · Low latency");
            if (t.containsIgnoreCase ("Exclusive"))        return S("WASAPI · Exclusive");
            if (t.containsIgnoreCase ("DirectSound"))      return S("DirectSound");
            if (t.containsIgnoreCase ("Windows Audio"))    return S("WASAPI");
            return t;
        }

        static constexpr int kMaxCatalogPorts = 240;   // 与 AudioEngine::kMaxPorts 留余量

        static void appendExtraPorts (std::vector<PortInfo>& ports, const DeviceEntry& d,
                                      int deviceIndex, bool input,
                                      const juce::String& deviceLabel)
        {
            const int total = input ? d.inputChannels : d.outputChannels;
            const auto& names = input ? d.inputChannelNames : d.outputChannelNames;
            for (int c = 2; c < total; c += 2)
            {
                for (int k = 0; k < 2 && c + k < total; ++k)
                {
                    if (static_cast<int> (ports.size()) >= kMaxCatalogPorts) return;
                    const int ci = c + k;
                    const juce::String cn = ci < names.size() && names[ci].isNotEmpty()
                                          ? names[ci] : (S("CH") + juce::String (ci + 1));
                    ports.push_back ({ deviceIndex, ci, input,
                                      d.displayName + S(" · ") + cn,
                                      d.displayName, d.typeName, cn, deviceLabel });
                }
            }
        }

        /** 读取辅助设备显示名单。规则（v2）：其它驱动设备默认一律不勾选，
            只有用户在设备设置页主动勾选并确定后才记录；主驱动通道由
            masterOff 名单单独管理，默认全部勾选。

            一次性迁移：旧版 auxHidden（隐藏名单=默认全可见）曾被反向迁移成
            「所有已枚举设备都勾选」的 v1 名单，会让其它设备整片默认勾上。
            凡是没有 version=2 标记的名单（含旧 auxHidden 存档、全新安装）
            全部重置为空，并写回一个带 version=2 的空名单，避免下次启动重试。 */
        void reloadAuxPrefs()
        {
            shownIn_.clear();
            shownOut_.clear();
            const auto off = DevicePreferences{}.loadMasterOff();
            masterOffIn_  = off.inputs;
            masterOffOut_ = off.outputs;
            auto xml = juce::parseXML (DevicePreferences::file());
            if (xml == nullptr) return;

            if (auto* e = xml->getChildByName ("auxShown"))
            {
                if (e->getIntAttribute ("version", 1) >= 2)
                {
                    for (auto* c : e->getChildIterator())
                    {
                        if (c->hasTagName ("in"))  shownIn_.add  (c->getStringAttribute ("name"));
                        if (c->hasTagName ("out")) shownOut_.add (c->getStringAttribute ("name"));
                    }
                    return;
                }
            }

            // 旧名单 / 旧 auxHidden / 无名单：一次性重置成「其它设备都不勾」。
            DevicePreferences{}.saveShown (DevicePreferences::ShownAux{});
        }

        std::vector<DeviceEntry> devices_;
        std::map<juce::String, juce::String> notes_;        // 设备备注：noteKey -> 备注名
        std::map<juce::String, DeviceEntry> probeCache_;   // ASIO 通道探测结果
        std::vector<PortInfo> inputPorts_, outputPorts_;
        juce::StringArray typeNames_;
        juce::StringArray shownIn_, shownOut_;         // 勾选显示的其它设备（键 = auxKey）
        juce::StringArray masterOffIn_, masterOffOut_; // 主设备被取消勾选的通道对（键 = auxKey|@pair）
    };
}
