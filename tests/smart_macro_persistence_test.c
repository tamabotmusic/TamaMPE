/* SPDX-License-Identifier: GPL-3.0-only */
/* Smart Macro routes in FUN20/FBKK and TPLM: no runtime values are stored. */
#define PERSISTENCE_TEST_NO_MAIN
#include "persistence_test.c"
#include <assert.h>

static void reset_smart(void)
{
    memset(trk, 0, sizeof trk);
    memset(smart_macro, 0, sizeof smart_macro);
    memset(smart_shadow, 0, sizeof smart_shadow);
    memset(smart_mpe_out, 0, sizeof smart_mpe_out);
    memset(smart_mpe_out_shadow, 0, sizeof smart_mpe_out_shadow);
    memset((void *)smart_publish_seq, 0, sizeof smart_publish_seq);
    memset((void *)smart_applied_seq, 0, sizeof smart_applied_seq);
    memset((void *)smart_reset_req, 0, sizeof smart_reset_req);
    memset(&tmpl, 0, sizeof tmpl);
    host_tracks_init();
}

static void maps_fill(void)
{
    for(uint32_t t=0;t<NTRK;t++)for(uint32_t m=0;m<SMART_MACRO_N;m++)
        smart_mpe_out_map_config(&trk[t],m,(m+t)%3u+1u,(int32_t)(m*41u+t*13u)-70,
                                 -(int32_t)(m+t+1u),(int32_t)(m+t+2u),(m+t)%3u);
}
static int maps_equal(void)
{
    for(uint32_t t=0;t<NTRK;t++)for(uint32_t m=0;m<SMART_MACRO_N;m++){
        const smart_mpe_out_map_t *q=&smart_mpe_out_shadow[t][m];
        if(q->depth!=(int32_t)(m*41u+t*13u)-70||q->lo!=-(int32_t)(m+t+1u)||q->hi!=(int32_t)(m+t+2u)||
           (q->meta&3u)!=(m+t)%3u+1u||((q->meta>>2)&3u)!=(m+t)%3u)return 0;
    } return 1;
}
static int maps_off(void)
{ for(uint32_t t=0;t<NTRK;t++)for(uint32_t m=0;m<SMART_MACRO_N;m++){const smart_mpe_out_map_t*q=&smart_mpe_out_shadow[t][m];if(q->depth||q->lo||q->hi||q->meta)return 0;}return 1; }

static void routes_fill(void)
{
    for (uint32_t t=0;t<NTRK;t++) for (uint32_t m=0;m<SMART_MACRO_N;m++) for (uint32_t r=0;r<SMART_ROUTE_N;r++)
        smart_macro_route_config(&trk[t],m,r,(m+r+t)&1u?SMART_DST_FM6_MLVL:SMART_DST_PAN,
                                 (int32_t)(t*17u+m*7u+r*3u)-31, -(int32_t)(m+r+1u), (int32_t)(t+m+r+2u),
                                 (m+r+t)%3u);
}

static int routes_equal(void)
{
    for (uint32_t t=0;t<NTRK;t++) for (uint32_t m=0;m<SMART_MACRO_N;m++) for (uint32_t r=0;r<SMART_ROUTE_N;r++) {
        const smart_route_t *q=smart_macro_route_get(&trk[t],m,r);
        int32_t d=(int32_t)(t*17u+m*7u+r*3u)-31,lo=-(int32_t)(m+r+1u),hi=(int32_t)(t+m+r+2u);
        if (!q || q->destination != ((m+r+t)&1u?SMART_DST_FM6_MLVL:SMART_DST_PAN) || q->depth != d || q->lo != lo || q->hi != hi || q->curve != (m+r+t)%3u)
            return 0;
    }
    return 1;
}

static int routes_off(void)
{
    for(uint32_t t=0;t<NTRK;t++)for(uint32_t m=0;m<SMART_MACRO_N;m++)for(uint32_t r=0;r<SMART_ROUTE_N;r++) {
        const smart_route_t *q=smart_macro_route_get(&trk[t],m,r);
        if(!q || q->destination || q->depth || q->lo || q->hi || q->curve) return 0;
    }
    return 1;
}

int main(void)
{
    static uint8_t raw[BANK_STORE_SIZE];
    uint8_t saved_smr1[PROJ_SMR1_SIZE], saved_sme1[PROJ_SME1_SIZE];
    uint8_t snapshot[PROJ_SMR1_SIZE];
    reset(); reset_smart(); routes_fill(); maps_fill();
    for(uint32_t t=0;t<NTRK;t++) smart_macro_value(&trk[t],0,91-(int32_t)t);
    project_capture(&proj_scratch);
    assert(bank_pack(raw,&proj_scratch,1) && bank_valid(raw,sizeof raw));
    assert(smart_u32_get(raw+8u+PROJ_SMR1_OFF)==SMART_SMR1_MAGIC);
    assert(smart_u32_get(raw+8u+PROJ_SME1_OFF)==SMART_SME1_MAGIC);
    /* SME1 is a fixed little-bit-endian 16 x 26-bit wire record.  These
     * known bytes protect the schema independently of a round trip. */
    { uint8_t sme[SMART_SME1_SIZE], smr[SMART_SMR1_SIZE];
      assert(smart_macro_snapshot_persist(smr,sme));
      assert(sizeof sme==62u && sme[0]=='S'&&sme[1]=='M'&&sme[2]=='E'&&sme[3]=='1'&&sme[4]==1u&&sme[5]==52u);
      /* track 0 macro 0: depth -70 -> 57, lo -1, hi 2, CC74, linear */
      assert(sme[6]==57u && sme[7]==0x01u && sme[8]==0x41u && !(sme[9]&3u));
      assert(smart_u32_get(sme+58u)==smart_crc32(sme,58u));
      sme[4]=2; smart_macro_restore_persist(smr,sme); assert(routes_equal()&&maps_off());
      /* SAVE/autosave must refuse to replace an opaque future extension. */
      assert(!smart_macro_snapshot_persist(smr,sme));
      smart_macro_restore_persist(raw+8u+PROJ_SMR1_OFF,raw+8u+PROJ_SME1_OFF);
      assert(smart_macro_snapshot_persist(smr,sme)); sme[6]=255u; smart_u32_put(sme+58u,smart_crc32(sme,58u)); smart_macro_restore_persist(smr,sme); assert(!smart_mpe_out_shadow[0][0].meta);
      assert(smart_macro_snapshot_persist(smr,sme)); sme[9]|=0x0cu; smart_u32_put(sme+58u,smart_crc32(sme,58u)); smart_macro_restore_persist(smr,sme); assert(!smart_mpe_out_shadow[0][0].meta);
      assert(smart_macro_snapshot_persist(smr,sme)); sme[58]^=1u; smart_macro_restore_persist(smr,sme); assert(routes_equal()&&maps_off());
      smart_macro_restore_persist(raw+8u+PROJ_SMR1_OFF,raw+8u+PROJ_SME1_OFF); }
    for(uint32_t t=0;t<NTRK;t++)smart_macro_clear_routes(&trk[t]);
    smart_macro_restore_persist(raw+8u+PROJ_SMR1_OFF,raw+8u+PROJ_SME1_OFF);
    assert(routes_equal()&&maps_equal());
    for(uint32_t t=0;t<NTRK;t++)for(uint32_t m=0;m<SMART_MACRO_N;m++)assert(!smart_macro_target(&trk[t],m));

    /* A bounded snapshot never consumes an editor's incomplete publication. */
    smart_edit_begin(0);
    assert(!smart_macro_snapshot_routes(snapshot));
    smart_edit_publish(0);
    assert(smart_macro_snapshot_routes(snapshot));

    /* A malformed extension is non-musical metadata: outer records remain
     * valid when a transport has recomputed their full checksums. */
    raw[8u+PROJ_SMR1_OFF+8u]^=1u;
    smart_u32_put(raw+8u+PROJ_STORE_SIZE-4u,proj_hash(raw+8u,PROJ_STORE_SIZE-4u));
    bank_checksum(raw);
    assert(bank_valid(raw,sizeof raw));
    smart_macro_restore_persist(raw+8u+PROJ_SMR1_OFF,raw+8u+PROJ_SME1_OFF);
    assert(routes_off()&&maps_equal());

    /* No SMR1 in a legacy FUN20/FBKK also defaults safely to OFF. */
    memset(raw+8u+PROJ_SMR1_OFF,0,PROJ_SMR1_SIZE);
    memset(raw+8u+PROJ_SME1_OFF,0,PROJ_SME1_SIZE);
    smart_u32_put(raw+8u+PROJ_STORE_SIZE-4u,proj_hash(raw+8u,PROJ_STORE_SIZE-4u));
    bank_checksum(raw); assert(bank_valid(raw,sizeof raw));
    smart_macro_restore_persist(raw+8u+PROJ_SMR1_OFF,raw+8u+PROJ_SME1_OFF); assert(routes_off()&&maps_off());

    /* A physical alteration remains a full-object error: neither FUN20 nor
     * FBKK accepts it when their original checksums are left intact. */
    raw[8u+PROJ_SMR1_OFF]^=1u;
    assert(!bank_valid(raw,sizeof raw));

    reset_smart(); routes_fill(); maps_fill();
    template_save(); assert(template_used() && tmpl.magic==TMPL_MAGIC_SME1);
    for(uint32_t t=0;t<NTRK;t++)smart_macro_clear_routes(&trk[t]);
    template_load(); assert(routes_equal()&&maps_equal());

    /* TPLJ remains a supported musical template and simply has no routes. */
    {
        tmplj_t old;
        memcpy(&old, &tmpl, __builtin_offsetof(tmpl_t, smr1));
        old.size = sizeof old; old.magic = TMPL_MAGIC;
        assert(tmpl_take((const uint8_t *)&old, sizeof old));
        template_load(); assert(routes_off()&&maps_off());
    }
    /* TPLK remains readable and initializes the absent SME1 safely OFF. */
    { tmplk_t old; memcpy(&old,&tmpl,sizeof old); old.size=sizeof old;old.magic=TMPL_MAGIC_SMR1;
      assert(tmpl_take((const uint8_t *)&old,sizeof old));template_load();assert(routes_off()&&maps_off()); }

    /* Rename must preserve the stored extension, not copy the live project. */
    reset(); reset_smart(); routes_fill(); maps_fill();
    assert(!project_save_as(0,"ROUTES"));
    assert(st_load(OBJ_BANK0,raw,sizeof raw)==(int)sizeof raw);
    /* Rename treats an extension as opaque.  A later SMR1 version therefore
     * survives a name-only change even though this firmware cannot apply it. */
    raw[8u+PROJ_SMR1_OFF+4u]=2u;
    smart_u32_put(raw+8u+PROJ_STORE_SIZE-4u,proj_hash(raw+8u,PROJ_STORE_SIZE-4u));
    bank_checksum(raw); assert(!st_save(OBJ_BANK0,raw,sizeof raw));
    memcpy(saved_smr1,raw+8u+PROJ_SMR1_OFF,sizeof saved_smr1);
    memcpy(saved_sme1,raw+8u+PROJ_SME1_OFF,sizeof saved_sme1);
    smart_macro_clear_routes(&trk[0]);
    assert(!project_save_as(1,"OTHER"));
    assert(!project_rename(0,"RENAMED"));
    assert(st_load(OBJ_BANK0,raw,sizeof raw)==(int)sizeof raw);
    assert(!memcmp(saved_smr1,raw+8u+PROJ_SMR1_OFF,sizeof saved_smr1));
    assert(!memcmp(saved_sme1,raw+8u+PROJ_SME1_OFF,sizeof saved_sme1));

    /* The normal NOR CRC covers SMR1.  A damaged newest A/B copy selects the
     * older complete project rather than accepting a partial extension. */
    reset(); reset_smart(); routes_fill(); maps_fill();
    assert(!project_save(0));
    smart_macro_route_config(&trk[0],0,0,SMART_DST_PAN,55,-4,5,SMART_CURVE_EXP);
    assert(!project_save(0));
    { st_hdr_t h; int copy=st_current(OBJ_BANK0,&h); assert(copy>=0);
      nor[st_sector(OBJ_BANK0,(uint32_t)copy)+ST_PAYLOAD_OFF+8u+PROJ_SMR1_OFF+8u]^=1u; }
    for(uint32_t t=0;t<NTRK;t++)smart_macro_clear_routes(&trk[t]);
    project_load(0); assert(routes_equal());

    /* Runtime knob positions do not dirty/autosave a project.  A route
     * change does, because the durable SMR1 bytes have changed. */
    { uint32_t before;
      before=erases;
      smart_macro_value(&trk[0],0,47);
      project_autosave(); assert(erases==before);
      smart_macro_route_config(&trk[0],0,0,SMART_DST_PAN,54,-4,5,SMART_CURVE_EXP);
      project_autosave(); assert(erases>before); }

    /* BOOT follows the normal full-project load path and must therefore
     * restore routes, but never the live knob positions. */
    reset(); reset_smart(); routes_fill(); maps_fill();
    assert(!project_save(0));
    smart_macro_value(&trk[0],0,80);
    for(uint32_t t=0;t<NTRK;t++)smart_macro_clear_routes(&trk[t]);
    settings_boot=1; bootguard.failed=0; project_boot();
    assert(routes_equal() && maps_equal() && !smart_macro_target(&trk[0],0));

    apply_preset_to(&trk[0],0); assert(smart_macro_route_get(&trk[0],0,0)->destination);
    project_new_blank(); assert(routes_off());
    puts("Smart Macro SMR1/SME1 project/template persistence and safe defaults OK");
    return 0;
}
