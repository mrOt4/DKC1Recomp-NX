/* Windows OpenGL implementation of the existing Mac presentation passes.
 * Only immutable completed host pixels are read. No guest state is touched. */
#include "windows_platform.h"
#include "desktop_crt.h"
#include "dkc1_hd_gpu.h"
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windows_shaders.h"

#define GL_API(X) \
 X(PFNGLCREATESHADERPROC,CreateShader) X(PFNGLSHADERSOURCEPROC,ShaderSource) \
 X(PFNGLCOMPILESHADERPROC,CompileShader) X(PFNGLGETSHADERIVPROC,GetShaderiv) \
 X(PFNGLGETSHADERINFOLOGPROC,GetShaderInfoLog) X(PFNGLDELETESHADERPROC,DeleteShader) \
 X(PFNGLCREATEPROGRAMPROC,CreateProgram) X(PFNGLATTACHSHADERPROC,AttachShader) \
 X(PFNGLLINKPROGRAMPROC,LinkProgram) X(PFNGLGETPROGRAMIVPROC,GetProgramiv) \
 X(PFNGLGETPROGRAMINFOLOGPROC,GetProgramInfoLog) X(PFNGLDELETEPROGRAMPROC,DeleteProgram) \
 X(PFNGLUSEPROGRAMPROC,UseProgram) X(PFNGLGETUNIFORMLOCATIONPROC,GetUniformLocation) \
 X(PFNGLUNIFORM1IVPROC,Uniform1iv) X(PFNGLUNIFORM1FVPROC,Uniform1fv) \
 X(PFNGLACTIVETEXTUREPROC,ActiveTexture) X(PFNGLGENVERTEXARRAYSPROC,GenVertexArrays) \
 X(PFNGLBINDVERTEXARRAYPROC,BindVertexArray) X(PFNGLDELETEVERTEXARRAYSPROC,DeleteVertexArrays) \
 X(PFNGLGENFRAMEBUFFERSPROC,GenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC,BindFramebuffer) \
 X(PFNGLFRAMEBUFFERTEXTURE2DPROC,FramebufferTexture2D) \
 X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,CheckFramebufferStatus) X(PFNGLDELETEFRAMEBUFFERSPROC,DeleteFramebuffers)
#define DECLARE(type,name) static type p##name;
GL_API(DECLARE)
#undef DECLARE
enum { Flat,Reconstruct,Lines,Beam,Down,Blur,Compose,Hd,PassCount };
typedef struct Target { GLuint texture; int w,h; } Target;
static SDL_Window *s_window;
static SDL_GLContext s_context;
static GLuint s_programs[PassCount],s_vao,s_fbo;
static Target s_input,s_hd_input,s_lines,s_beam,s_glow[2],s_halo[2];
static const char *vertex_source=
  "#version 330 core\nout vec2 v_uv;\n"
  "uniform int final_pass;\n"
  "void main(){vec2 p[4]=vec2[](vec2(-1,-1),vec2(-1,1),vec2(1,-1),vec2(1,1));"
  "vec2 t[4]=vec2[](vec2(0,0),vec2(0,1),vec2(1,0),vec2(1,1));"
  "v_uv=t[gl_VertexID]; if(final_pass!=0) v_uv.y=1-v_uv.y;"
  "gl_Position=vec4(p[gl_VertexID],0,1);}";

static GLuint Shader(GLenum type,const char *source) {
  GLuint shader=pCreateShader(type); pShaderSource(shader,1,&source,NULL);
  pCompileShader(shader); GLint ok=0; pGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
  if (!ok) { char error[4096]; pGetShaderInfoLog(shader,sizeof error,NULL,error);
    fprintf(stderr,"Windows shader compile failure: %s\n",error); pDeleteShader(shader); return 0; }
  return shader;
}
static bool Ensure(Target *target,int w,int h,bool input) {
  if (target->texture && target->w==w && target->h==h) return true;
  if (!target->texture) glGenTextures(1,&target->texture);
  target->w=w; target->h=h; glBindTexture(GL_TEXTURE_2D,target->texture);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D,0,input ? GL_RGBA8 : GL_RGBA16F,w,h,0,GL_BGRA,GL_UNSIGNED_BYTE,NULL);
  return glGetError()==GL_NO_ERROR;
}
static void Bind(int unit,Target *target,bool linear) {
  pActiveTexture(GL_TEXTURE0+unit); glBindTexture(GL_TEXTURE_2D,target->texture);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear ? GL_LINEAR : GL_NEAREST);
}
static bool Pass(int index,Target *source,Target *target,int x,int y,int w,int h,float *u) {
  pBindFramebuffer(GL_FRAMEBUFFER,target ? s_fbo : 0);
  if (target) {
    pFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target->texture,0);
    if (pCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return false;
  }
  glViewport(x,y,w,h); pUseProgram(s_programs[index]);
  u[0]=(float)source->w; u[1]=(float)source->h;
  pUniform1fv(pGetUniformLocation(s_programs[index],"u"),27,u);
  int final=target==NULL; pUniform1iv(pGetUniformLocation(s_programs[index],"final_pass"),1,&final);
  bool linear=index==Down || index==Blur || index==Compose || index==Hd || (index==Flat && u[4]!=0);
  Bind(0,source,linear);
  if (index==Compose) { Bind(1,&s_glow[0],true); Bind(2,&s_halo[0],true); }
  pBindVertexArray(s_vao); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  return glGetError()==GL_NO_ERROR;
}
bool Dkc1WindowsGraphicsInit(SDL_Window *window) {
  s_window=window; s_context=SDL_GL_CreateContext(window);
  if (!s_context) return false;
#define LOAD(type,name) p##name=(type)SDL_GL_GetProcAddress("gl" #name); if(!p##name)return false;
  GL_API(LOAD)
#undef LOAD
  GLuint vertex=Shader(GL_VERTEX_SHADER,vertex_source); if(!vertex)return false;
  for(int i=0;i<PassCount;i++) {
    GLuint fragment=Shader(GL_FRAGMENT_SHADER,kWindowsShaders[i]); if(!fragment)return false;
    GLuint program=pCreateProgram(); pAttachShader(program,vertex); pAttachShader(program,fragment);
    pLinkProgram(program); pDeleteShader(fragment); GLint ok=0; pGetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok) { char error[4096];pGetProgramInfoLog(program,sizeof error,NULL,error);
      fprintf(stderr,"Windows shader link failure: %s\n",error);return false; }
    s_programs[i]=program; pUseProgram(program);
    const char *names[]={"source","lines","image","glow","halo"};
    const int units[]={0,0,0,1,2};
    for(int j=0;j<5;j++) pUniform1iv(pGetUniformLocation(program,names[j]),1,&units[j]);
  }
  pDeleteShader(vertex); pGenVertexArrays(1,&s_vao);pGenFramebuffers(1,&s_fbo);
  SDL_GL_SetSwapInterval(0); /* QPC is the single 60 Hz producer authority. */
  /* HD texture frames are composed on the GPU unless DKC1_HD_GPU=0. */
  {
    const char *gpu=getenv("DKC1_HD_GPU");
    if(!(gpu&&*gpu=='0')&&Dkc1HdGpuInit(false))Dkc1HdSetGpuComposition(true);
  }
  fprintf(stderr,"[windows-graphics] OpenGL %s; all %d shader passes ready\n",glGetString(GL_VERSION),(int)PassCount);
  return true;
}
void Dkc1WindowsGraphicsDraw(const uint32_t *pixels,int w,int h,int display_width,const Dkc1GraphicsSettings *settings) {
  if(!s_context)return;
  SDL_GL_MakeCurrent(s_window,s_context);
  int ow,oh;SDL_GL_GetDrawableSize(s_window,&ow,&oh);if(ow<1||oh<1)return;
  int vw=ow,vh=ow*h/display_width;
  if(vh>oh){vh=oh;vw=oh*display_width/h;}
  int x=(ow-vw)/2,y=(oh-vh)/2;
  pBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,ow,oh);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  pActiveTexture(GL_TEXTURE0);
  if(!Ensure(&s_input,w,h,true))return;
  glBindTexture(GL_TEXTURE_2D,s_input.texture);
  glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
  float u[27]={(float)w,(float)h,(float)vw,(float)vh,(float)settings->reconstruct_mode,
    settings->strength/100.f,settings->softness/100.f,settings->shading/100.f};
  Dkc1CrtFrameParams c;
  bool ok=true;
  if(settings->display==kDkc1DisplayCrt && Dkc1CrtDerive(&settings->crt,vw,vh,w,h,&c)) {
    int gw=vw/4>0?vw/4:1,gh=vh/4>0?vh/4:1,hw=vw/16>0?vw/16:1,hh=vh/16>0?vh/16:1;
    ok=Ensure(&s_lines,vw,h,false)&&Ensure(&s_beam,vw,vh,false);
    for(int i=0;i<2;i++)ok=ok&&Ensure(&s_glow[i],gw,gh,false)&&Ensure(&s_halo[i],hw,hh,false);
    u[8]=c.sigma_h;u[9]=c.sigma_dark;u[10]=c.sigma_bright;u[11]=c.beam_fade;
    u[14]=c.glow;u[15]=c.halation;u[16]=c.curvature_x;u[17]=c.curvature_y;
    u[18]=c.corner_radius;u[19]=c.vignette;u[20]=c.mask==kDkc1CrtMaskNone?0.f:c.mask==kDkc1CrtMaskSlot?2.f:1.f;
    u[21]=(float)c.mask_pitch;u[22]=c.mask_strength;u[23]=c.mask_gain;u[24]=c.knee;
    ok=ok&&Pass(Lines,&s_input,&s_lines,0,0,vw,h,u)&&Pass(Beam,&s_lines,&s_beam,0,0,vw,vh,u);
    for(int i=0;i<2;i++) {
      Target *source=i?&s_glow[0]:&s_beam,*a=i?&s_halo[0]:&s_glow[0],*b=i?&s_halo[1]:&s_glow[1];
      ok=ok&&Pass(Down,source,a,0,0,a->w,a->h,u);
      u[12]=1;u[13]=0;ok=ok&&Pass(Blur,a,b,0,0,b->w,b->h,u);
      u[12]=0;u[13]=1;ok=ok&&Pass(Blur,b,a,0,0,a->w,a->h,u);
    }
    ok=ok&&Pass(Compose,&s_beam,NULL,x,y,vw,vh,u);
  }else{
    if(settings->upscaler!=kDkc1UpscalerReconstruct)u[4]=(float)settings->upscaler;
    ok=Pass(settings->upscaler==kDkc1UpscalerReconstruct?Reconstruct:Flat,&s_input,NULL,x,y,vw,vh,u);
  }
  if(!ok)fprintf(stderr,"[windows-graphics] render pass failed\n");
}
/* HD texture surface (runner/dkc1_hd.c): w x h pixels that represent a
 * logical display_width x logical_height picture. Fitted like the native
 * frame; Reconstruct/CRT do not apply to an already upscaled image. */
void Dkc1WindowsGraphicsDrawHd(const uint32_t *pixels,int w,int h,int display_width,int logical_height) {
  if(!s_context||display_width<1||logical_height<1)return;
  SDL_GL_MakeCurrent(s_window,s_context);
  int ow,oh;SDL_GL_GetDrawableSize(s_window,&ow,&oh);if(ow<1||oh<1)return;
  int vw=ow,vh=ow*logical_height/display_width;
  if(vh>oh){vh=oh;vw=oh*display_width/logical_height;}
  int x=(ow-vw)/2,y=(oh-vh)/2;
  pBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,ow,oh);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  pActiveTexture(GL_TEXTURE0);
  if(!Ensure(&s_hd_input,w,h,true))return;
  glBindTexture(GL_TEXTURE_2D,s_hd_input.texture);
  glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
  float u[27]={(float)w,(float)h,(float)vw,(float)vh};
  if(!Pass(Hd,&s_hd_input,NULL,x,y,vw,vh,u))fprintf(stderr,"[windows-graphics] HD pass failed\n");
}
/* HD texture frame composed on the GPU (dkc1_hd_gpu.c), fitted like the
 * native picture. Returns false when composition failed. */
bool Dkc1WindowsGraphicsDrawHdGpu(const Dkc1HdGpuInputs *in,int display_width,int logical_height) {
  if(!s_context||!in||display_width<1||logical_height<1)return false;
  SDL_GL_MakeCurrent(s_window,s_context);
  int ow,oh;SDL_GL_GetDrawableSize(s_window,&ow,&oh);if(ow<1||oh<1)return false;
  int vw=ow,vh=ow*logical_height/display_width;
  if(vh>oh){vh=oh;vw=oh*display_width/logical_height;}
  if(!Dkc1HdGpuCompose(in))return false;
  pBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,ow,oh);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  Dkc1HdGpuPresent(0,(ow-vw)/2,(oh-vh)/2,vw,vh);
  pBindVertexArray(s_vao);
  return true;
}
void Dkc1WindowsGraphicsSwap(void){if(s_context)SDL_GL_SwapWindow(s_window);}
void Dkc1WindowsGraphicsClose(void){
  if(!s_context)return;
  Dkc1HdGpuShutdown();
  Target *targets[]={&s_input,&s_hd_input,&s_lines,&s_beam,&s_glow[0],&s_glow[1],&s_halo[0],&s_halo[1]};
  for(int i=0;i<8;i++){glDeleteTextures(1,&targets[i]->texture);memset(targets[i],0,sizeof(Target));}
  for(int i=0;i<PassCount;i++){pDeleteProgram(s_programs[i]);s_programs[i]=0;}
  pDeleteVertexArrays(1,&s_vao);pDeleteFramebuffers(1,&s_fbo);SDL_GL_DeleteContext(s_context);s_context=NULL;
}
int Dkc1WindowsGraphicsTest(void){
  SDL_Window *window=SDL_CreateWindow("Synthetic graphics test",0,0,256,224,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
  if(!window||!Dkc1WindowsGraphicsInit(window))return 1;
  uint32_t pixels[256*224],copy[256*224];uint8_t rgb[256*224*3],repeat[256*224*3];
  for(int y=0;y<224;y++)for(int x=0;x<256;x++)pixels[y*256+x]=0xff000000u|((uint32_t)x<<16)|((uint32_t)y<<8)|(((x+y)&1)?80:180);
  memcpy(copy,pixels,sizeof copy);Dkc1GraphicsSettings s;Dkc1GraphicsDefault(&s);
  for(int mode=0;mode<12;mode++){
    s.display=mode>=9;s.upscaler=mode<4?mode:2;s.reconstruct_mode=mode<4?3:(mode-4)%5;
    if(mode>=9)Dkc1CrtSettingsApplyPreset(&s.crt,mode-9);
    Dkc1WindowsGraphicsDraw(pixels,256,224,256,&s);glReadPixels(0,0,256,224,GL_RGB,GL_UNSIGNED_BYTE,rgb);
    if(glGetError()!=GL_NO_ERROR)return 2;
    if(mode==0)for(int y=0;y<224;y++)for(int x=0;x<256;x++){
      uint32_t p=pixels[y*256+x];size_t j=((223-y)*256+x)*3;
      if(rgb[j]!=(uint8_t)(p>>16)||rgb[j+1]!=(uint8_t)(p>>8)||rgb[j+2]!=(uint8_t)p)return 3;
    }
    Dkc1WindowsGraphicsDraw(pixels,256,224,256,&s);glReadPixels(0,0,256,224,GL_RGB,GL_UNSIGNED_BYTE,repeat);
    if(memcmp(rgb,repeat,sizeof rgb)||memcmp(pixels,copy,sizeof copy))return 4;
  }
  /* Exercise real upscaling, including fractional drawable geometry and
   * same-size source replacement. Native-size tests alone can hide no-ops. */
  const int sizes[][2]={{1024,896},{913,701}};
  for(int size=0;size<2;size++){
    int width=sizes[size][0],height=sizes[size][1];
    SDL_SetWindowSize(window,width,height);
    SDL_GL_GetDrawableSize(window,&width,&height);
    size_t bytes=(size_t)width*height*4;
    uint8_t *base=malloc(bytes),*current=malloc(bytes),*again=malloc(bytes);
    if(!base||!current||!again)return 5;
    for(int mode=0;mode<12;mode++){
      Dkc1GraphicsDefault(&s);s.display=mode>=9;
      s.upscaler=mode<4?mode:kDkc1UpscalerReconstruct;
      s.reconstruct_mode=mode<4?3:(mode-4)%5;
      if(mode>=9)Dkc1CrtSettingsApplyPreset(&s.crt,mode-9);
      Dkc1WindowsGraphicsDraw(pixels,256,224,256,&s);
      glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,current);
      if(glGetError()!=GL_NO_ERROR)return 6;
      Dkc1WindowsGraphicsDraw(pixels,256,224,256,&s);
      glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,again);
      if(memcmp(current,again,bytes)||memcmp(pixels,copy,sizeof copy))return 7;
      if(!mode)memcpy(base,current,bytes);
      /* Sharp bilinear and sharp-pixel reconstruction can match nearest at
       * an integer scale. CRT must visibly transform this source. */
      else if(mode>=9&&!memcmp(base,current,bytes))return 8;
    }
    for(size_t i=0;i<256*224;i++)pixels[i]^=0x00ffffffu;
    Dkc1WindowsGraphicsDraw(pixels,256,224,256,&s);
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,again);
    if(!memcmp(current,again,bytes))return 9;
    memcpy(pixels,copy,sizeof copy);free(base);free(current);free(again);
  }
  Dkc1WindowsGraphicsClose();SDL_DestroyWindow(window);
  puts("WINDOWS_GRAPHICS_PASS: 12 modes at native, 4x and fractional sizes; exact native RGB; stable repeats; immutable input; source invalidation");return 0;
}
