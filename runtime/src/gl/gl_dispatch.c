/* GL import name table.
 *
 * imports.c forwards every unresolved "gl*" symbol here; the table maps the
 * original GLES1 entry point name to its host implementation.
 */
#include "gl_internal.h"

typedef struct { const char *name; void (*fn)(CPU *); } GlEnt;
static const GlEnt g_gl[] = {
    {"glClearColor", gl_ClearColor}, {"glClearDepthf", gl_ClearDepthf},
    {"glColor4f", gl_Color4f}, {"glColor4ub", gl_Color4ub},
    {"glTranslatef", gl_Translatef}, {"glRotatef", gl_Rotatef},
    {"glLoadMatrixf", gl_LoadMatrixf}, {"glMatrixMode", gl_MatrixMode},
    {"glLoadIdentity", gl_LoadIdentity}, {"glPushMatrix", gl_PushMatrix},
    {"glPopMatrix", gl_PopMatrix}, {"glViewport", gl_Viewport},
    {"glScissor", gl_Scissor}, {"glClear", gl_Clear},
    {"glEnable", gl_Enable}, {"glDisable", gl_Disable},
    {"glEnableClientState", gl_EnableClientState}, {"glDisableClientState", gl_DisableClientState},
    {"glBlendFunc", gl_BlendFunc}, {"glAlphaFunc", gl_AlphaFunc},
    {"glShadeModel", gl_ShadeModel}, {"glDepthFunc", gl_DepthFunc},
    {"glDepthMask", gl_DepthMask}, {"glColorMask", gl_ColorMask},
    {"glCullFace", gl_CullFace}, {"glFrontFace", gl_FrontFace},
    {"glHint", gl_Hint}, {"glFinish", gl_Finish}, {"glPixelStorei", gl_PixelStorei},
    {"glFogf", gl_Fogf}, {"glFogfv", gl_Fogfv},
    {"glLightf", gl_Lightf}, {"glLightfv", gl_Lightfv}, {"glLightModelfv", gl_LightModelfv},
    {"glMaterialf", gl_Materialf}, {"glMaterialfv", gl_Materialfv},
    {"glTexEnvi", gl_TexEnvi}, {"glTexEnvfv", gl_TexEnvfv},
    {"glActiveTexture", gl_ActiveTexture}, {"glClientActiveTexture", gl_ClientActiveTexture},
    {"glBindTexture", gl_BindTexture}, {"glGenTextures", gl_GenTextures},
    {"glDeleteTextures", gl_DeleteTextures}, {"glTexParameteri", gl_TexParameteri},
    {"glTexParameterf", gl_TexParameterf}, {"glTexImage2D", gl_TexImage2D},
    {"glTexSubImage2D", gl_TexSubImage2D}, {"glCopyTexImage2D", gl_CopyTexImage2D},
    {"glCompressedTexImage2D", gl_CompressedTexImage2D}, {"glBindBuffer", gl_BindBuffer},
    {"glDrawArrays", gl_DrawArrays}, {"glDrawElements", gl_DrawElements},
    {"glVertexPointer", gl_VertexPointer}, {"glNormalPointer", gl_NormalPointer},
    {"glColorPointer", gl_ColorPointer}, {"glTexCoordPointer", gl_TexCoordPointer},
    {"glCurrentPaletteMatrixOES", gl_CurrentPaletteMatrixOES},
    {"glMatrixIndexPointerOES", gl_MatrixIndexPointerOES},
    {"glWeightPointerOES", gl_WeightPointerOES},
    {"glGetFloatv", gl_GetFloatv}, {"glGetError", gl_GetError}, {"glGetString", gl_GetString},
    {"glColor4x", gl_Color4x}, {"glTranslatex", gl_Translatex}, {"glRotatex", gl_Rotatex},
    {"glScalef", gl_Scalef}, {"glNormal3f", gl_Normal3f}, {"glMultMatrixf", gl_MultMatrixf},
    {"glDepthRangef", gl_DepthRangef}, {"glFrustumx", gl_Frustumx}, {"glOrthox", gl_Orthox},
    {"glClipPlanef", gl_ClipPlanef}, {"glLineWidth", gl_LineWidth}, {"glPointSize", gl_PointSize},
    {"glPointParameterf", gl_PointParameterf}, {"glPolygonOffset", gl_PolygonOffset},
    {"glStencilFunc", gl_StencilFunc}, {"glStencilMask", gl_StencilMask},
    {"glStencilOp", gl_StencilOp}, {"glTexEnvf", gl_TexEnvf}, {"glTexEnvx", gl_TexEnvx},
    {"glFlush", gl_Flush}, {"glGetBooleanv", gl_GetBooleanv}, {"glGetIntegerv", gl_GetIntegerv},
    {"glIsEnabled", gl_IsEnabled}, {"glReadPixels", gl_ReadPixels},
    {"glGenBuffers", gl_GenBuffers}, {"glDeleteBuffers", gl_DeleteBuffers},
    {"glBufferData", gl_BufferData}, {"glBufferSubData", gl_BufferSubData},
    {"glGenFramebuffersOES", gl_GenFramebuffersOES},
    {"glDeleteFramebuffersOES", gl_DeleteFramebuffersOES},
    {"glBindFramebufferOES", gl_BindFramebufferOES},
    {"glCheckFramebufferStatusOES", gl_CheckFramebufferStatusOES},
    {"glGenRenderbuffersOES", gl_GenRenderbuffersOES},
    {"glDeleteRenderbuffersOES", gl_DeleteRenderbuffersOES},
    {"glBindRenderbufferOES", gl_BindRenderbufferOES},
    {"glRenderbufferStorageOES", gl_RenderbufferStorageOES},
    {"glFramebufferRenderbufferOES", gl_FramebufferRenderbufferOES},
    {"glFramebufferTexture2DOES", gl_FramebufferTexture2DOES},
};

int gles1_dispatch(CPU *cpu, const char *name)
{
    for (size_t i = 0; i < sizeof(g_gl) / sizeof(g_gl[0]); i++)
        if (!strcmp(name, g_gl[i].name)) {
            if (strcmp(name, "glGetError")) g_last_gl = name;
            GLRec *r = &g_ring[g_ring_pos % GLR_MAX];
            g_ring_pos++;
            r->name = name;
            for (int j = 0; j < 8; j++) r->a[j] = cpu->r[j];
            g_gl[i].fn(cpu); return 1;
        }
    return 0;
}
