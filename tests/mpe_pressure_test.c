/* SPDX-License-Identifier: GPL-3.0-only */
#define MPE_NOTES_NO_MAIN 1
#include "mpe_notes_test.c"
#include <math.h>
#define NSIGNAL 4096
static int32_t signals[3][NSIGNAL];
static void pressure(uint32_t ch,uint32_t p){queued(0xD0u|ch,p,0,1);}
static void settle(void){for(uint32_t i=0;i<256;i++){int32_t o[CTL];mod_begin(&trk[0]);track_render(&trk[0],o,CTL);mod_end(&trk[0]);}}
static void capture(voice_t *v,const fm6_voice_t *saved,uint32_t p,int32_t *out)
{
    *fm6_state(&trk[0],v)=*saved;
    pressure(1,p);v->mpe_pressure_level=v->mpe_pressure_target;
    vmod_t m={.amp0=32767,.amp1=32767,.shape=64<<8};
    for(uint32_t at=0;at<NSIGNAL;at+=CTL){
        memset(out+at,0,CTL*sizeof *out);fm6_render(&trk[0],v,out+at,CTL,&m);fm6_join();
        for(uint32_t i=0;i<CTL;i++){
            assert(isfinite(fm6_sum[i]) && fabsf(fm6_sum[i])<16.0f); /* before output clipping */
            assert(abs(out[at+i])<32768);
        }
    }
}
static double harmonic(const int32_t *s,double hz)
{
    double re=0,im=0;
    for(uint32_t i=0;i<NSIGNAL;i++){
        double window=0.5-0.5*cos(6.283185307179586*i/(NSIGNAL-1));
        double a=6.283185307179586*hz*i/FS;
        re+=s[i]*window*cos(a);im+=s[i]*window*sin(a);
    }
    return re*re+im*im;
}
static double difference(const int32_t *a,const int32_t *b)
{
    double d=0,e=0;for(uint32_t i=0;i<NSIGNAL;i++){double x=a[i]-b[i];d+=x*x;e+=(double)a[i]*a[i];}
    return e?sqrt(d/e):0;
}
int main(void)
{
    uint32_t changed=0;
    for(uint32_t alg=0;alg<32;alg++){
        setup();uint8_t raw[FP_SIZE];fm6_unpack(FM6_INIT,raw);raw[FP_ALG]=(uint8_t)alg;raw[FP_FB]=0;
        for(uint32_t k=0;k<6;k++){
            uint8_t *op=raw+k*FP_OP;op[FP_OL]=fm6_carrier(alg,6-k)?80:60;op[FP_FC]=1;
            op[FP_AMS]=3;for(uint32_t j=0;j<3;j++)op[FP_L1+j]=99;
        }
        fm6_put_patch(0,raw,1);events_block(CTL);FM6F(&trk[0])[FN_ENGINE]=FM6_MODERN;
        queued(0x91,60,100,1);queued(0x92,60,100,1);settle();
        voice_t *a=sounding(owner(1,60)),*b=sounding(owner(2,60));
        fm6_voice_t saved=*fm6_state(&trk[0],a);
        fm6_ctl_t cache=FM6P(&trk[0])->pt;
        capture(a,&saved,0,signals[0]);capture(a,&saved,64,signals[1]);capture(a,&saved,127,signals[2]);
        double d=difference(signals[0],signals[2]);if(d>0.01)changed++;
        if(alg==31)assert(!memcmp(signals[0],signals[2],sizeof signals[0]));
        if(alg==0){
            double h0=harmonic(signals[0],2*261.63),h64=harmonic(signals[1],2*261.63),h127=harmonic(signals[2],2*261.63);
            printf("algorithm 1: harmonic-2 energy %.3g / %.3g / %.3g; relative PCM difference %.3f\n",h0,h64,h127,d);
            assert(difference(signals[0],signals[1])>0.01 && d>0.01);
            assert(fabs(h127-h0)>h0*0.01);
        }
        assert(b->mpe_pressure==0 && !b->mpe_pressure_target);
        assert(cache.amd==FM6P(&trk[0])->pt.amd && cache.pok==FM6P(&trk[0])->pt.pok);
        assert(!memcmp(cache.pt,FM6P(&trk[0])->pt.pt,sizeof cache.pt));
        /* Modern operator gain is exp2(level-14): carriers unchanged, active
         * modulators rise by sqrt(2), independent of the shared AMS cache. */
        *fm6_state(&trk[0],a)=saved;a->mpe_pressure_level=0;a->mpe_pressure_target=0;
        vmod_t m={.shape=64<<8};fm6_control(&trk[0],a,fm6_state(&trk[0],a),&m);
        float gains[6];memcpy(gains,fm6_state(&trk[0],a)->gout,sizeof gains);
        *fm6_state(&trk[0],a)=saved;a->mpe_pressure_level=a->mpe_pressure_target=32768;
        fm6_control(&trk[0],a,fm6_state(&trk[0],a),&m);
        for(uint32_t k=0;k<6;k++){
            double ratio=fm6_state(&trk[0],a)->gout[k]/gains[k];
            assert(fabs(ratio-(fm6_carrier(alg,6-k)?1:sqrt(2)))<0.0001);
        }
    }
    assert(changed>20);printf("controlled algorithms: %u/32 with >1%% PCM change; carrier-only case intentionally unchanged\n",changed);
    for(uint32_t preset=0;preset<8;preset++){
        setup();host_preset(&trk[0],ENGI_FM6,preset);trk[0].p[P_VOICE]=V_POLY;events_block(CTL);configure(3);
        queued(0x91,60,100,1);settle();voice_t *a=sounding(owner(1,60));fm6_voice_t s=*fm6_state(&trk[0],a);
        uint8_t patch[sizeof fm6_patch];memcpy(patch,fm6_patch,sizeof patch);
        capture(a,&s,0,signals[0]);capture(a,&s,64,signals[1]);capture(a,&s,127,signals[2]);
        char name[11];fm6_name(name,fm6_patch[0]);
        printf("DX7 preset %u (%s): pressure64/127 relative waveform change %.3f / %.3f\n",preset,name,difference(signals[0],signals[1]),difference(signals[0],signals[2]));
        assert(!memcmp(patch,fm6_patch,sizeof patch));

    }
    setup();queued(0x91,60,100,1);voice_t *a=sounding(owner(1,60));pressure(1,127);
    int32_t previous=0;
    for(uint32_t i=0;i<128;i++){
        vmod_t m={.shape=64<<8};fm6_control(&trk[0],a,fm6_state(&trk[0],a),&m);
        assert(a->mpe_pressure_level>=previous && a->mpe_pressure_level<=32768);previous=a->mpe_pressure_level;
    }
    assert(a->mpe_pressure_level==32768);pressure(1,0);settle();assert(!a->mpe_pressure_level);
    pressure(0,127);assert(!a->mpe_pressure_target && !trk[0].at);
    pressure(1,64);queued(0xB0,64,127,1);queued(0x81,60,0,1);uint16_t frozen=a->mpe_pressure_target;
    pressure(1,0);queued(0x91,64,100,1);voice_t *b=sounding(owner(1,64));pressure(1,127);
    assert(a->mpe_pressure_target==frozen && b->mpe_pressure_target==32768);
    queued(0xB0,64,0,1);pressure(1,0);assert(a->mpe_pressure_target==frozen);
    note_id_t stolen=owner(1,64);voice_kill(b);queued(0x91,67,100,1);b=sounding(owner(1,67));
    pressure(1,64);queued(0x81,64,0,1);assert(b->gate && b->mpe_pressure==64);
    queued(0xB1,121,0,1);assert(!b->mpe_pressure_target);configure(0);assert(!owners()&&!identities());
    puts("smoothing, master isolation, sustain/release, reuse, stolen identities and cleanup OK");
    /* No resurrection: compare planned gains against pressure zero for zero
     * patch level, disabled switches, finished and below-threshold envelopes. */
    for(uint32_t condition=0;condition<4;condition++){
        setup();uint8_t raw[FP_SIZE];fm6_unpack(FM6_INIT,raw);raw[0*FP_OP+FP_OL]=80;
        if(!condition)raw[0*FP_OP+FP_OL]=0;
        fm6_put_patch(0,raw,1);events_block(CTL);FM6F(&trk[0])[FN_ENGINE]=FM6_MODERN;
        queued(0x91,60,100,1);settle();a=sounding(owner(1,60));fm6_voice_t *s=fm6_state(&trk[0],a);
        if(condition==1)fm6_on[0]&=~1u;
        if(condition==2)s->eg[0].ix=4;
        if(condition==3){s->eg[0].level=0;s->eg[0].hold=1<<30;}
        if(!condition){fm6_voice_t init=*s;capture(a,&init,0,signals[0]);capture(a,&init,127,signals[2]);assert(!memcmp(signals[0],signals[2],sizeof signals[0]));}
        fm6_voice_t save=*s;vmod_t m={.shape=64<<8};
        a->mpe_pressure_level=a->mpe_pressure_target=0;fm6_control(&trk[0],a,s,&m);float gain=s->gout[0];uint8_t quiet=s->quiet;
        *s=save;a->mpe_pressure_level=a->mpe_pressure_target=32768;fm6_control(&trk[0],a,s,&m);
        assert(s->gout[0]==gain && s->quiet==quiet);
    }
    mpe_clear(1);puts("zero-level/disabled/finished/quiet modulators remain unchanged; no envelope lifetime extension OK");
    return 0;
}
