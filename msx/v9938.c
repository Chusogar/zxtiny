// v9938.c
#include "v9938.h"
#include <string.h>

static inline uint32_t vmask(uint32_t a){ return a & (V9938_VRAM_SIZE-1); }

static inline uint32_t rgb333(uint8_t r,uint8_t g,uint8_t b){
    r=(r*255)/7; g=(g*255)/7; b=(b*255)/7;
    return 0xFF000000 | (r<<16)|(g<<8)|b;
}

static void pal_update(V9938* v,int i){
    uint16_t p=v->pal_rgb[i];
    v->pal_argb[i]=rgb333((p>>6)&7,(p>>3)&7,p&7);
}

void v9938_reset(V9938* v){
    memset(v,0,sizeof(*v));

    static const uint16_t defpal[16]={
        0x000,0x000,0x109,0x1DB,0x05E,0x07F,0x149,0x1F6,
        0x16C,0x1EE,0x194,0x1B5,0x0C9,0x17A,0x155,0x1FF
    };
    for(int i=0;i<16;i++){
        v->pal_rgb[i]=defpal[i];
        pal_update(v,i);
    }
}

static void set_addr(V9938* v,uint8_t hi){
    uint32_t a=((v->reg[14]&7)<<14)|((hi&0x3F)<<8)|v->latch;
    v->addr=a&0x1FFFF;
    v->write_mode=((hi>>6)&3)==1;

    if(!v->write_mode){
        v->read_buf=v->vram[vmask(v->addr)];
        v->addr=(v->addr+1)&0x1FFFF;
    }
}

static uint8_t vram_read(V9938* v){
    uint8_t o=v->read_buf;
    v->read_buf=v->vram[vmask(v->addr)];
    v->addr=(v->addr+1)&0x1FFFF;
    v->second=false;
    return o;
}

static void vram_write(V9938* v,uint8_t val){
    v->vram[vmask(v->addr)]=val;
    v->read_buf=val;
    v->addr=(v->addr+1)&0x1FFFF;
    v->second=false;
}

static uint8_t status_read(V9938* v){
    uint8_t s=v->stat[v->reg[15]&0x0F];
    if((v->reg[15]&0x0F)==0){
        v->stat[0]&=0x1F;
        v->irq=false;
    }
    v->second=false;
    return s;
}

static void reg_write(V9938* v,uint8_t r,uint8_t val){
    v->reg[r&0x3F]=val;
    if((r&0x3F)==1){
        if((v->reg[1]&0x20)&&(v->stat[0]&0x80))
            v->irq=true;
    }
}

static void palette_write(V9938* v,uint8_t val){
    uint8_t idx=v->reg[16]&0x0F;
    if(!v->pal_second){
        v->pal_latch=val;
        v->pal_second=true;
    }else{
        v->pal_second=false;
        uint8_t r=(v->pal_latch>>4)&7;
        uint8_t b=v->pal_latch&7;
        uint8_t g=val&7;
        v->pal_rgb[idx]=(r<<6)|(g<<3)|b;
        pal_update(v,idx);
        v->reg[16]=(v->reg[16]&0xF0)|((idx+1)&0x0F);
    }
}

static void reg_indirect(V9938* v,uint8_t val){
    uint8_t r=v->reg[17]&0x3F;
    if(r!=17) reg_write(v,r,val);
    if(!(v->reg[17]&0x80))
        v->reg[17]=(v->reg[17]&0xC0)|((r+1)&0x3F);
}

uint8_t v9938_read(V9938* v,uint8_t p){
    if(p==0x98) return vram_read(v);
    if(p==0x99) return status_read(v);
    return 0xFF;
}

void v9938_write(V9938* v,uint8_t p,uint8_t val){
    if(p==0x98){ vram_write(v,val); return; }

    if(p==0x99){
        if(!v->second){ v->latch=val; v->second=true; }
        else{
            v->second=false;
            if((val&0xC0)==0x80) reg_write(v,val&0x3F,v->latch);
            else set_addr(v,val);
        }
        return;
    }
    if(p==0x9A){ palette_write(v,val); return; }
    if(p==0x9B){ reg_indirect(v,val); return; }
}

static int decode_mode(V9938* v){
    int m1=(v->reg[1]>>4)&1;
    int m2=(v->reg[1]>>3)&1;
    int m3=(v->reg[0]>>1)&1;
    int m4=(v->reg[0]>>2)&1;
    int m5=(v->reg[0]>>3)&1;
    return (m5<<4)|(m4<<3)|(m3<<2)|(m2<<1)|m1;
}

void v9938_render_frame(V9938* v,uint32_t* fb,int w,int h){
    int lines=(v->reg[9]&0x80)?212:192;
    if(lines>h) lines=h;

    uint32_t bg=v->pal_argb[v->reg[7]&0x0F];
    for(int i=0;i<w*h;i++) fb[i]=bg;

    if(!(v->reg[1]&0x40)) return;

    uint32_t base=((v->reg[2]&0x7F)<<10)&0x1FFFF;
    int mode=decode_mode(v);

    if(mode==0b01100){ // G4
        for(int y=0;y<lines;y++){
            uint32_t a=base+y*128;
            for(int x=0;x<256;x+=2){
                uint8_t b=v->vram[vmask(a++)];
                fb[y*w+x]=v->pal_argb[b>>4];
                fb[y*w+x+1]=v->pal_argb[b&0xF];
            }
        }
    }
    if(mode==0b11100){ // G7
        for(int y=0;y<lines;y++){
            uint32_t a=base+y*256;
            for(int x=0;x<256;x++){
                uint8_t b=v->vram[vmask(a+x)];
                fb[y*w+x]=rgb333(b>>5,(b>>2)&7,(b&3)*2);
            }
        }
    }

    v->stat[0]|=0x80;
    if(v->reg[1]&0x20) v->irq=true;
}

