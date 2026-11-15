#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>

#include "I18n.h"
#include "Theme.h"

//==============================================================================
// VST3 插件编辑器窗口：双击通道条插件行（或菜单「打开界面」）弹出。
// 生命周期归 MainComponent（OwnedArray），关闭按钮异步移除自身。
//==============================================================================
class PluginWindow : public juce::DocumentWindow
{
public:
    std::function<void()> onClose;

    PluginWindow (juce::AudioPluginInstance* p)
        : DocumentWindow (p->getName(), mb::ui::topBar, DocumentWindow::allButtons, true),
          owner_ (p)
    {
        // 内容组件（尤其 VST3 的原生子窗口）就绪前先别显示：否则会先弹出一个
        // 纯色空窗，插件视图随后 attach 时偶发跟丢父 HWND，表现为「空白页面」。
        setVisible (false);
        setUsingNativeTitleBar (true);
        setColour (backgroundColourId, mb::ui::card);

        int ew = 380, eh = 180;
        if (auto* ed = p->createEditorIfNeeded())
        {
            setContentNonOwned (ed, true);
            setResizable (ed->isResizable(), false);
            ew = juce::jmax (320, ed->getWidth());
            eh = juce::jmax (220, ed->getHeight());
            ed->setVisible (true);
        }
        else
        {
            // createEditor() 返回 nullptr 时旧版直接显示一块空白，无从判断原因。
            placeholder_ = std::make_unique<Placeholder>();
            setContentNonOwned (placeholder_.get(), false);
            setResizable (false, false);
        }

        centreWithSize (ew, eh);
        resized();
        setVisible (true);
        toFront (true);
    }

    ~PluginWindow() override
    {
        if (auto* ed = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
            owner_->editorBeingDeleted (ed);
        clearContentComponent();
    }

    bool isFor (const juce::AudioPluginInstance* p) const noexcept { return owner_ == p; }

    void closeButtonPressed() override
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PluginWindow> (this)]
        {
            if (safe != nullptr && safe->onClose != nullptr)
                safe->onClose();
        });
    }

private:
    struct Placeholder : public juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            g.fillAll (mb::ui::card);
            g.setColour (mb::ui::subText);
            g.setFont (mb::ui::font (13.0f));
            g.drawText (mb::tr ("插件界面创建失败，请重新插入该插件后重试",
                                "Could not create the plugin editor. Please re-insert the plugin."),
                        getLocalBounds().reduced (20), juce::Justification::centred, true);
        }
    };

    juce::AudioPluginInstance* owner_;
    std::unique_ptr<Placeholder> placeholder_;   // 仅 createEditor 失败时使用

    JUCE_DECLARE_NON_COPYABLE (PluginWindow)
};
