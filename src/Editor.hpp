#pragma once
#include "Parameters.hpp"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cbitmap.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/controls/cknob.h"
#include "vstgui/lib/controls/ctextlabel.h"
#include <charconv>
#include <string>
#include "vstgui/uidescription/uiattributes.h"
#include <array>

namespace chopper {
// Format only the on-panel readout. Preserve SDK parameter precision,
// automation, state serialization and the DSP mapping without changes.
class PercentLabel final:public VSTGUI::CTextLabel{
public:
    PercentLabel():CTextLabel(VSTGUI::CRect(0,0,1,1)){}
    void setText(const VSTGUI::UTF8String& txt) override{
        auto text=txt.getString();
        for(auto& c:text)if(c==',')c='.';
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
public:
    Preview(const VSTGUI::CRect& r,Steinberg::Vst::EditController* c):CView(r),controller(c){
        setMouseEnabled(false);setWantsIdle(true);refresh();
    }
    void onIdle() override { refresh(); }
    void draw(VSTGUI::CDrawContext* c) override {
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
    void refresh(){
        Parameters p;
        constexpr std::array<Param,7> ids{Param::start,Param::mid,Param::end,Param::length,Param::position,Param::curve,Param::width};
        std::array<double,ids.size()> now{};
        for(std::size_t i=0;i<ids.size();++i){
            now[i]=controller->getParamNormalized(static_cast<Steinberg::Vst::ParamID>(ids[i]));
            p.set(ids[i],now[i]);
        }
        if(initialized&&now==previous)return;
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
        invalid();
    }
    Steinberg::Vst::EditController* controller;
    std::array<double,7> previous{};
    std::array<double,4096> starts{},ends{};
    std::size_t count=0;
    double position=.5;
    bool initialized=false;
};
class Editor final:public VSTGUI::AspectRatioVST3Editor{
public:
    explicit Editor(Steinberg::Vst::EditController* c):AspectRatioVST3Editor(c,"view","chopper.uidesc"){
        setMinZoomFactor(.75);enableTooltips(true);setIdleRate(33);
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
        return AspectRatioVST3Editor::open(parent,type);
    }
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* r) override{
        if(!r)return Steinberg::kInvalidArgument;
        // SDK AspectRatioVST3Editor applies zoom but does not update the
        // CPluginView rectangle. Forward the accepted geometry to the
        // standard VST3Editor path as well; it handles resize reentry.
        const auto result=AspectRatioVST3Editor::onSize(r);
        if(result!=Steinberg::kResultTrue)return result;
        return VSTGUI::VST3Editor::onSize(r);
    }
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* r) override{
        if(!r)return Steinberg::kInvalidArgument;
        const auto maxWidth=static_cast<Steinberg::int32>(std::lround(1440*getContentScaleFactor()));
        if(r->getWidth()>maxWidth)r->right=r->left+maxWidth;
        return AspectRatioVST3Editor::checkSizeConstraint(r);
    }
    VSTGUI::CView* createView(const VSTGUI::UIAttributes& attrs,const VSTGUI::IUIDescription* desc) override{
        const auto* name=attrs.getAttributeValue("custom-view-name");
        if(name&&*name=="PercentReadout")return new PercentLabel();
        if(name&&*name=="ChopperPreview")return new Preview(VSTGUI::CRect(24,72,936,210),getController());
        return AspectRatioVST3Editor::createView(attrs,desc);
    }
};
}
