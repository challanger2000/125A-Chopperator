#pragma once
#include "Parameters.hpp"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cbitmap.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/controls/cknob.h"
#include "vstgui/uidescription/uiattributes.h"
#include <array>

namespace chopper {
class Preview final:public VSTGUI::CView {
public:
    Preview(const VSTGUI::CRect& r,Steinberg::Vst::EditController* c):CView(r),controller(c){
        setMouseEnabled(false);setWantsIdle(true);refresh();
    }
    void onIdle() override { refresh(); }
    void draw(VSTGUI::CDrawContext* c) override {
        using namespace VSTGUI;
        const auto r=getViewSize();
        c->setFillColor(CColor(16,19,21,255));c->drawRect(r,kDrawFilled);
        c->setFrameColor(CColor(43,48,52,255));c->setLineWidth(1);
        const double l=r.left+20,rr=r.right-20,top=r.top+38,bottom=r.bottom-20;
        for(int i=0;i<3;++i){double y=top+(bottom-top)*i/2;c->drawLine({l,y},{rr,y});}
        c->drawLine({l,top},{l,bottom});c->drawLine({rr,top},{rr,bottom});
        double mid=l+(rr-l)*position;
        c->drawLine({mid,top},{mid,bottom});
        c->setFont(kNormalFontSmall);
        c->setFontColor(CColor(180,184,188,255));
        c->drawString("START",CPoint(l, r.top+18));
        c->drawString("MID",CPoint(mid-10,r.top+18));
        c->drawString("END",CPoint(rr-22,r.top+18));
        c->drawString("PREVIEW  120 BPM / 4/4",CRect(l,r.bottom-17,rr,r.bottom-2),kCenterText);
        c->setFrameColor(CColor(245,156,73,255));c->setLineWidth(1.2);
        double last=0;
        for(std::size_t i=0;i<low.size();++i){
            double x=l+(rr-l)*double(i)/double(low.size()-1);
            double y1=bottom-(bottom-top)*low[i],y2=bottom-(bottom-top)*high[i];
            if(i)c->drawLine({l+(rr-l)*double(i-1)/double(low.size()-1),last},{x,y2});
            c->drawLine({x,y1},{x,y2});last=y1;
        }
        setDirty(false);
    }
private:
    void refresh(){
        Parameters p;std::array<double,11> now{};
        for(std::size_t i=0;i<now.size();++i){
            now[i]=controller->getParamNormalized(static_cast<Steinberg::Vst::ParamID>(100+i));
            p.set(static_cast<Param>(100+i),now[i]);
        }
        if(initialized&&now==previous)return;
        previous=now;initialized=true;
        const auto s=p.settings();position=s.position;
        // A deliberately low-rate, fixed reference clock for visual preview only.
        // Use the actual DSP core (including attack/release and mix).
        constexpr double sr=2000;
        Core core;core.prepare(sr);core.configure(s);core.setClock(120,4,4);core.setActive(true);
        const auto count=static_cast<int>(s.bars*2*sr);
        low.fill(1);high.fill(0);
        for(int i=0;i<count;++i){
            const auto j=std::min(low.size()-1,static_cast<std::size_t>(i)*low.size()/static_cast<std::size_t>(count));
            const double g=core.tick();low[j]=std::min(low[j],g);high[j]=std::max(high[j],g);
        }
        for(std::size_t i=0;i<low.size();++i)if(low[i]>high[i])low[i]=high[i]=i?high[i-1]:1;
        invalid();
    }
    Steinberg::Vst::EditController* controller;
    std::array<double,11> previous{};
    std::array<double,872> low{},high{};
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
        if(name&&*name=="ChopperPreview")return new Preview(VSTGUI::CRect(24,72,936,210),getController());
        return AspectRatioVST3Editor::createView(attrs,desc);
    }
};
}
