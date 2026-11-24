#pragma once
#include "Parameters.hpp"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cbitmap.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/events.h"
#include "vstgui/lib/cvstguitimer.h"
#include <vector>
#include <functional>
#include "vstgui/lib/controls/cknob.h"
#include "vstgui/lib/controls/ctextlabel.h"
#include <charconv>
#include <string>
#include "vstgui/uidescription/uiattributes.h"
#include <array>

namespace chopper {
struct EditorPreferences {
    double zoom=1.; bool help=false;
    // GUI-thread-only registry. An editor can be created, removed or reopened
    // independently of the processing component. No audio-thread accesses.
    struct Watch { const void* owner; std::function<void()> update; };
    std::vector<Watch> watches;
    void notify(){for(auto& w:watches)w.update();}
    void remove(const void* owner){std::erase_if(watches,[owner](const auto& w){return w.owner==owner;});}
};
// Format only the on-panel readout. Preserve SDK parameter precision,
// automation, state serialization and the DSP mapping without changes.
class PercentLabel final:public VSTGUI::CTextLabel{
public:
    PercentLabel():CTextLabel(VSTGUI::CRect(0,0,1,1)){}
    void setText(const VSTGUI::UTF8String& txt) override{
        auto text=txt.getString();
        for(auto& c:text)if(c==',')c='.';
        const auto first=text.find_first_not_of(" \t");
        if(first!=std::string::npos)text.erase(0,first);
        const auto last=text.find_last_not_of(" %\t");
        if(last!=std::string::npos)text.resize(last+1);
        double value=0;
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
        if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||!std::isfinite(value)){
            CTextLabel::setText(txt);return;
        }
        if(std::abs(value)<.05)value=0;
        char buffer[32]{};
        const auto formatted=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::fixed,1);
        if(formatted.ec!=std::errc{}){CTextLabel::setText(txt);return;}
        std::string display(buffer,formatted.ptr);
        if(display.size()>=2&&display.substr(display.size()-2)==".0")display.resize(display.size()-2);
        display+="%";
        CTextLabel::setText(VSTGUI::UTF8String(display));
    }
};
class Preview final:public VSTGUI::CView {
    static constexpr std::array<Param,7> ids{Param::start,Param::mid,Param::end,Param::length,Param::position,Param::curve,Param::width};
public:
    Preview(const VSTGUI::CRect& r,Steinberg::Vst::EditController* c,EditorPreferences& p):CView(r),controller(c),prefs(p){
        setMouseEnabled(false);setTransparency(false);refresh(false);
    }
    bool attached(VSTGUI::CView* parent) override{
        if(!CView::attached(parent))return false;
        prefs.watches.push_back({this,[this]{pending=true;}});
        // Use a dedicated GUI timer rather than the SDK global idle updater.
        // A dirty bit coalesces rapid drags/automation to one render per frame.
        timer=VSTGUI::owned(new VSTGUI::CVSTGUITimer([this](VSTGUI::CVSTGUITimer*){
            if(refresh(false)||pending){pending=false;repaint();}
        },33,true));
        refresh(false);return true;
    }
    bool removed(VSTGUI::CView* parent) override{
        if(timer){timer->stop();timer=nullptr;}
        prefs.remove(this);
        return CView::removed(parent);
    }
    void draw(VSTGUI::CDrawContext* c) override {
        refresh(false);
        using namespace VSTGUI;
        const auto r=getViewSize();
        // Owner's Unbenannt.svg: viewBox 340 x 110. This is a timing
        // diagram, not an audio oscilloscope or gain-envelope plot.
        const auto x=[&](double v){return r.left+r.getWidth()*v/340;};
        const auto y=[&](double v){return r.top+r.getHeight()*v/110;};
        c->setFillColor(CColor(16,17,19,255));c->drawRect(r,kDrawFilled);
        c->setFrameColor(CColor(69,69,69,255));c->setLineWidth(1);
        c->drawRect(CRect(x(1),y(1),x(339),y(109)),kDrawStroked);
        c->setFrameColor(CColor(52,52,52,255));
        for(int i=0;i<=4;++i){
            const double xx=x(10+80*i);
            for(int v=8;v<100;v+=6)c->drawLine({xx,y(v)},{xx,y(std::min(100,v+3))});
        }
        c->setFillColor(CColor(232,155,64,255));
        for(std::size_t i=0;i<count;++i){
            const double left=x(10+320*starts[i]),right=x(10+320*ends[i]);
            if(right>left)c->drawRect(CRect(left,y(16),right,y(69)),kDrawFilled);
        }
        c->setFont(kNormalFontSmall);c->setFontColor(CColor(170,170,170,255));
        c->drawString("START",CRect(x(10),y(78),x(70),y(98)),kLeftText);
        c->drawString("MID",CRect(x(10+320*position-28),y(78),x(10+320*position+28),y(98)),kCenterText);
        c->drawString("END",CRect(x(270),y(78),x(330),y(98)),kRightText);
        setDirty(false);
    }
private:
    void repaint(){
        // Invalidate the whole logical panel via its parent, including the
        // preview area. Avoid partial-composition clipping/stale regions.
        if(auto* parent=getParentView())parent->invalid();
    }
    bool refresh(bool redraw){
        Parameters p;
        std::array<double,ids.size()> now{};
        for(std::size_t i=0;i<ids.size();++i){
            now[i]=controller->getParamNormalized(static_cast<Steinberg::Vst::ParamID>(ids[i]));
            p.set(ids[i],now[i]);
        }
        if(initialized&&now==previous)return false;
        previous=now;initialized=true;
        const auto s=p.settings();position=s.position;
        // In 4/4 one bar contains one whole note. Invert the same
        // integrated phase law used by Core for each gate edge.
        // No sample-drop or fictitious gaps; WIDTH defines the open fraction
        // in phase units, not an approximate screen-space percentage.
        const double total=s.bars*integral(s,1);
        const auto inverse=[&](double phase){
            if(phase<=0)return 0.;
            if(phase>=total)return 1.;
            double lo=0,hi=1;
            for(int k=0;k<28;++k){
                const double mid=(lo+hi)*.5;
                if(s.bars*integral(s,mid)<phase)lo=mid;else hi=mid;
            }
            return (lo+hi)*.5;
        };
        count=0;
        if(s.width>0){
            for(std::size_t cycle=0;cycle<starts.size()&&double(cycle)<total;++cycle){
                starts[count]=inverse(double(cycle));
                ends[count]=inverse(double(cycle)+s.width);
                ++count;
            }
        }
        if(redraw)repaint();return true;
    }
    Steinberg::Vst::EditController* controller;
    EditorPreferences& prefs;
    VSTGUI::SharedPointer<VSTGUI::CVSTGUITimer> timer;
    bool pending=false;
    std::array<double,7> previous{};
    std::array<double,4096> starts{},ends{};
    std::size_t count=0;
    double position=.5;
    bool initialized=false;
};

// Editor-only controls: no VST3 parameter, automation or DSP mutation.
class HelpButton final:public VSTGUI::CView{
public:
    HelpButton(bool initial,std::function<void(bool)> cb):CView(VSTGUI::CRect(496,20,520,44)),change(std::move(cb)),enabled(initial){}
    void draw(VSTGUI::CDrawContext* c) override{
        using namespace VSTGUI;
        auto r=getViewSize();r.inset(2,2);
        c->setFillColor(enabled?CColor(232,155,64,255):CColor(36,39,42,255));
        c->setFrameColor(CColor(109,109,109,255));c->setLineWidth(1);c->drawEllipse(r,kDrawFilledAndStroked);
        c->setFont(kNormalFont);c->setFontColor(enabled?CColor(16,17,19,255):CColor(226,227,228,255));
        c->drawString("?",r,kCenterText);setDirty(false);
    }
    void onMouseDownEvent(VSTGUI::MouseDownEvent& e) override{
        if(e.buttonState.isLeft()){enabled=!enabled;change(enabled);invalid();e.consumed=true;}
    }
private:
    std::function<void(bool)> change;
    bool enabled=false;
};
class ResizeGrip final:public VSTGUI::CView{
public:
    ResizeGrip(std::function<double()> s,std::function<void(double)> cb):
        CView(VSTGUI::CRect(940,556,960,576)),scale(std::move(s)),resize(std::move(cb)){}
    void draw(VSTGUI::CDrawContext* c) override{
        using namespace VSTGUI;auto r=getViewSize();
        c->setFrameColor(CColor(168,170,172,255));c->setLineWidth(1.2);
        for(int i:{5,10,15})c->drawLine({r.right-i-2,r.bottom-3},{r.right-3,r.bottom-i-2});
        setDirty(false);
    }
    void onMouseEnterEvent(VSTGUI::MouseEnterEvent&) override{if(getFrame())getFrame()->setCursor(VSTGUI::kCursorNWSESize);}
    void onMouseExitEvent(VSTGUI::MouseExitEvent&) override{if(getFrame()&&!dragging)getFrame()->setCursor(VSTGUI::kCursorDefault);}
    void onMouseDownEvent(VSTGUI::MouseDownEvent& e) override{
        if(!e.buttonState.isLeft())return;
        dragging=true;initialWidth=960*scale();anchor=e.mousePosition.x*scale();e.consumed=true;
    }
    void onMouseMoveEvent(VSTGUI::MouseMoveEvent& e) override{
        if(!dragging)return;
        // Events arrive in the scaled logical view coordinates. Convert to
        // physical width with the current scale to avoid compounded deltas.
        resize(initialWidth+e.mousePosition.x*scale()-anchor);e.consumed=true;
    }
    void onMouseUpEvent(VSTGUI::MouseUpEvent& e) override{
        if(dragging){dragging=false;if(getFrame())getFrame()->setCursor(VSTGUI::kCursorDefault);e.consumed=true;}
    }
    void onMouseCancelEvent(VSTGUI::MouseCancelEvent& e) override{dragging=false;if(getFrame())getFrame()->setCursor(VSTGUI::kCursorDefault);e.consumed=true;}
private:
    std::function<double()> scale;
    std::function<void(double)> resize;
    double initialWidth=960,anchor=0;
    bool dragging=false;
};

class Editor final:public VSTGUI::AspectRatioVST3Editor{
public:
    explicit Editor(Steinberg::Vst::EditController* c,EditorPreferences& p):AspectRatioVST3Editor(c,"view","chopper.uidesc"),preferences(p){
        setMinZoomFactor(.75);enableTooltips(p.help);
        setZoomFactor(p.zoom);
        rect.right=static_cast<Steinberg::int32>(std::lround(960*p.zoom));
        rect.bottom=static_cast<Steinberg::int32>(std::floor(rect.right*.6));
    }
    // Linear drag is the primary input, independent of the host preference.
    int32_t getKnobMode() const override{return VSTGUI::kLinearMode;}
protected:
    VSTGUI::CView* verifyView(VSTGUI::CView* view,const VSTGUI::UIAttributes& attrs,const VSTGUI::IUIDescription* desc) override{
        auto* result=AspectRatioVST3Editor::verifyView(view,attrs,desc);
        if(auto* knob=dynamic_cast<VSTGUI::CKnobBase*>(result)){
            // A full LENGTH sweep takes 195 logical pixels: three per step.
            // The three short note lists and continuous controls use 150 px.
            knob->setKnobRange(knob->getTag()==103?195.f:150.f);
            knob->setZoomFactor(5.f);
            knob->setWantsFocus(true);
        }
        if(auto* control=dynamic_cast<VSTGUI::CControl*>(result)){
            static constexpr const char* tips[]{
                "START: initial rhythmic note division.",
                "MID: note division at the middle point.",
                "END: final rhythmic note division.",
                "LENGTH: duration of the pattern in bars.",
                "MID POSITION: location of the middle division within the pattern.",
                "CURVE: shape of the rate transition. 0% is linear.",
                "SHAPE: waveform of each rhythmic impulse.",
                "WIDTH: open fraction of each impulse. 50% means equal open and closed time.",
                "ATTACK: fade-in time relative to the impulse period.",
                "RELEASE: fade-out time relative to the impulse period.",
                "MIX: dry/wet balance. 0% is dry.",
                "ACTIVE: starts the pattern on an OFF to ON transition.",
                "MODE: play once or repeat continuously."
            };
            const auto tag=control->getTag();
            if(tag>=100&&tag<=112)control->setTooltipText(tips[tag-100]);
        }
        return result;
    }
    bool PLUGIN_API open(void* parent,const VSTGUI::PlatformType& type) override{
        // Fail explicitly rather than presenting a blank or low-resolution editor.
        auto* desc=getUIDescription();
        for(const char* name:{"knobL","knobS"}){
            auto* b=dynamic_cast<VSTGUI::CMultiFrameBitmap*>(desc->getBitmap(name));
            if(!b||!b->isLoaded()||b->getNumFrames()!=128||b->getNumFramesPerRow()!=16)return false;
            for(double scale:{.5,.75,1.,1.5}){
                auto selected=b->getBestPlatformBitmapForScaleFactor(scale);
                if(!selected||selected->getScaleFactor()!=scale)return false;
            }
        }
        auto* logo=desc->getBitmap("logo");
        if(!logo||!logo->isLoaded()||logo->getWidth()!=84||logo->getHeight()!=42)return false;
        const bool ok=AspectRatioVST3Editor::open(parent,type);
        opened=ok;return ok;
    }
    void PLUGIN_API close() override{opened=false;AspectRatioVST3Editor::close();}
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* r) override{
        if(!r)return Steinberg::kInvalidArgument;
        // On initial attachment and reentrant host acknowledgement, use
        // the SDK base path without overwriting the saved editor preferences.
        if(!opened||resizing)return VSTGUI::VST3Editor::onSize(r);
        const auto oldZoom=getZoomFactor();
        const auto factor=std::clamp(r->getWidth()/(960.*getContentScaleFactor()),.75,1.5);
        if(std::abs(r->getHeight()-r->getWidth()*.6)>1.01)return Steinberg::kResultFalse;
        resizing=true;
        setZoomFactor(factor);
        const auto result=VSTGUI::VST3Editor::onSize(r);
        if(result==Steinberg::kResultTrue)preferences.zoom=factor;
        else setZoomFactor(oldZoom);
        resizing=false;return result;
    }
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* r) override{
        if(!r)return Steinberg::kInvalidArgument;
        const auto scale=getContentScaleFactor();
        const auto w=std::clamp(r->getWidth(),static_cast<Steinberg::int32>(std::lround(720*scale)),static_cast<Steinberg::int32>(std::lround(1440*scale)));
        r->right=r->left+w;r->bottom=r->top+static_cast<Steinberg::int32>(std::floor(w*.6));
        return Steinberg::kResultTrue;
    }
    VSTGUI::CView* createView(const VSTGUI::UIAttributes& attrs,const VSTGUI::IUIDescription* desc) override{
        const auto* name=attrs.getAttributeValue("custom-view-name");
        if(name&&*name=="HelpButton")return new HelpButton(preferences.help,[this](bool enabled){preferences.help=enabled;enableTooltips(enabled);});
        if(name&&*name=="ResizeGrip")return new ResizeGrip([this]{return getZoomFactor()*getContentScaleFactor();},[this](double width){
            Steinberg::ViewRect r{0,0,static_cast<Steinberg::int32>(std::lround(std::clamp(width,720*getContentScaleFactor(),1440*getContentScaleFactor()))),0};
            r.bottom=static_cast<Steinberg::int32>(std::floor(r.right*.6));
            if(checkSizeConstraint(&r)==Steinberg::kResultTrue)requestResize(VSTGUI::CPoint(r.getWidth(),r.getHeight()));
        });
        if(name&&*name=="PercentReadout")return new PercentLabel();
        if(name&&*name=="ChopperPreview")return new Preview(VSTGUI::CRect(24,72,936,210),getController(),preferences);
        return AspectRatioVST3Editor::createView(attrs,desc);
    }
private:
    EditorPreferences& preferences;
    bool opened=false,resizing=false;
};
}
