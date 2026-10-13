/* SPDX-License-Identifier: GPL-3.0-only */
#define DUAL_CORE_TEST_NO_MAIN 1
#include "dual_core_test.c"

static void pressure_case(int parallel,uint32_t alg,uint32_t engine)
{
    mpe_clear(1);memset(&mpe_zone,0,sizeof mpe_zone);setup_case(alg,engine,0);
    serial_render=!parallel;audio_worker_online=1;mi_r=mi_w=0;mpe_configure(3);
    midi_event(0x90,1,60,100);midi_event(0x90,2,60,110);
    for(uint32_t pos=0;pos<FRAMES;pos+=CTL){
        int32_t pcm[CTL*2];
        if(pos==CTL*16){midi_event(0xD0,1,0,0);midi_event(0xD0,2,64,0);}
        if(pos==CTL*48){midi_event(0xE0,1,127,127);midi_event(0xE0,2,0,0);}
        if(pos==CTL*64){midi_event(0xB0,0,64,127);midi_event(0x80,1,60,0);}
        if(pos==CTL*80){midi_event(0x90,1,64,110);midi_event(0xD0,1,0,0);}
        if(pos==CTL*112){midi_event(0xD0,1,64,0);midi_event(0xD0,2,0,0);}
        if(pos==CTL*144)midi_event(0xB0,0,64,0);
        if(pos==CTL*160)midi_event(0xB0,0,123,0);
        mix_block(pcm,CTL);assert(!fm6_pending);
        if(!parallel)memcpy(reference+2*pos,pcm,sizeof pcm);
        else assert(!memcmp(reference+2*pos,pcm,sizeof pcm));
    }
    mpe_clear(1);
}
int main(void)
{
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker_thread,NULL));
    while(!AW_LOAD(&audio_worker.ready))sched_yield();uint32_t before=fm6_pairs;
    for(uint32_t engine=0;engine<3;engine++)for(uint32_t alg=0;alg<32;alg++){
        pressure_case(0,alg,engine);pressure_case(1,alg,engine);
    }
    assert(fm6_pairs>before);assert(!pthread_cancel(thread));assert(!pthread_join(thread,NULL));
    printf("MPE pressure+bend: 96 serial/CPU1 PCM cases identical; %u actual pairs\n",fm6_pairs-before);
    return 0;
}
