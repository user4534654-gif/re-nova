/* GLES1 entry points that the Asphalt 5 library did not import.
 *
 * N.O.V.A. calls a few more GLES1 functions (fixed-point matrix/colour
 * variants, stencil, VBO/FBO, glReadPixels, ...).  These are thin wrappers
 * over desktop GL; the OES/EXT framebuffer tokens share their numeric values,
 * so they pass straight through.
 */

#include "gl_internal.h"

/* MinGW's <GL/gl.h> stops at GL 1.1 and lacks these pointer-sized types. */
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

/* ---- dynamically resolved desktop entry points ------------------------ */

typedef void (APIENTRY *PFNGLGENBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLBUFFERDATA)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (APIENTRY *PFNGLBUFFERSUBDATA)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef void (APIENTRY *PFNGLDELETEBUFFERS)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNGLPOINTPARAMETERF)(GLenum, GLfloat);
typedef void (APIENTRY *PFNGLGENFRAMEBUFFERSEXT)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLBINDFRAMEBUFFEREXT)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLDELETEFRAMEBUFFERSEXT)(GLsizei, const GLuint *);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSEXT)(GLenum);
typedef void (APIENTRY *PFNGLGENRENDERBUFFERSEXT)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLBINDRENDERBUFFEREXT)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLDELETERENDERBUFFERSEXT)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNGLRENDERBUFFERSTORAGEEXT)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFNGLFRAMEBUFFERRENDERBUFFEREXT)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DEXT)(GLenum, GLenum, GLenum, GLuint, GLint);

#define RESOLVE(var, type, name) \
    do { if (!(var)) (var) = (type)glproc(name); } while (0)

static PFNGLGENBUFFERS     pGenBuffers;
static PFNGLBUFFERDATA     pBufferData;
static PFNGLBUFFERSUBDATA  pBufferSubData;
static PFNGLDELETEBUFFERS  pDeleteBuffers;
static PFNGLPOINTPARAMETERF pPointParameterf;
static PFNGLGENFRAMEBUFFERSEXT pGenFramebuffers;
static PFNGLBINDFRAMEBUFFEREXT pBindFramebuffer;
static PFNGLDELETEFRAMEBUFFERSEXT pDeleteFramebuffers;
static PFNGLCHECKFRAMEBUFFERSTATUSEXT pCheckFramebufferStatus;
static PFNGLGENRENDERBUFFERSEXT pGenRenderbuffers;
static PFNGLBINDRENDERBUFFEREXT pBindRenderbuffer;
static PFNGLDELETERENDERBUFFERSEXT pDeleteRenderbuffers;
static PFNGLRENDERBUFFERSTORAGEEXT pRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFEREXT pFramebufferRenderbuffer;
static PFNGLFRAMEBUFFERTEXTURE2DEXT pFramebufferTexture2D;

/* ---- fixed-point (16.16) wrappers ------------------------------------- */

static float fx(uint32_t v) { return (float)((int32_t)v) / 65536.0f; }

void gl_Color4x(CPU *c)  { glColor4f(fx(A(0)), fx(A(1)), fx(A(2)), fx(A(3))); }
void gl_Translatex(CPU *c) { glTranslatef(fx(A(0)), fx(A(1)), fx(A(2))); }
void gl_Rotatex(CPU *c)  { glRotatef(fx(A(0)), fx(A(1)), fx(A(2)), fx(A(3))); }
void gl_Scalef(CPU *c)   { glScalef(fx(A(0)), fx(A(1)), fx(A(2))); }
void gl_Normal3f(CPU *c) { glNormal3f(fx(A(0)), fx(A(1)), fx(A(2))); }
void gl_MultMatrixf(CPU *c) { glMultMatrixf((const GLfloat *)AP(0)); }
void gl_DepthRangef(CPU *c) { glDepthRange(fx(A(0)), fx(A(1))); }
void gl_Frustumx(CPU *c)
{
    glFrustum(fx(A(0)), fx(A(1)), fx(A(2)), fx(A(3)), fx(A(4)), fx(A(5)));
}
void gl_Orthox(CPU *c)
{
    if (getenv("A5_GLDBG")) {
        static double lo[6] = { 1e30, 1e30, 1e30, 1e30, 1e30, 1e30 };
        static int n = 0;
        double v[6];
        for (int i = 0; i < 6; i++) v[i] = fx(A(i));
        if (n < 80 && (v[0] != lo[0] || v[1] != lo[1] || v[2] != lo[2] ||
                       v[3] != lo[3] || v[4] != lo[4] || v[5] != lo[5])) {
            fprintf(stderr, "[gldbg] Orthox %.1f %.1f %.1f %.1f %.1f %.1f\n",
                    v[0], v[1], v[2], v[3], v[4], v[5]);
            for (int i = 0; i < 6; i++) lo[i] = v[i];
            n++;
        }
    }
    glOrtho(fx(A(0)), fx(A(1)), fx(A(2)), fx(A(3)), fx(A(4)), fx(A(5)));
}
void gl_ClipPlanef(CPU *c)
{
    const GLfloat *p = (const GLfloat *)AP(1);
    GLdouble eq[4];
    if (p) for (int i = 0; i < 4; i++) eq[i] = p[i];
    glClipPlane((GLenum)A(0), eq);
}

/* ---- misc state ------------------------------------------------------- */

void gl_LineWidth(CPU *c)  { glLineWidth(AF(0)); }
void gl_PointSize(CPU *c)  { glPointSize(AF(0)); }
void gl_PointParameterf(CPU *c)
{
    RESOLVE(pPointParameterf, PFNGLPOINTPARAMETERF, "glPointParameterf");
    if (pPointParameterf) pPointParameterf((GLenum)A(0), AF(1));
}
void gl_PolygonOffset(CPU *c) { glPolygonOffset(AF(0), AF(1)); }
void gl_StencilFunc(CPU *c)
{
    glStencilFunc((GLenum)A(0), (GLint)A(1), (GLuint)A(2));
}
void gl_StencilMask(CPU *c) { glStencilMask((GLuint)A(0)); }
void gl_StencilOp(CPU *c)
{
    glStencilOp((GLenum)A(0), (GLenum)A(1), (GLenum)A(2));
}
void gl_TexEnvf(CPU *c) { glTexEnvf((GLenum)A(0), (GLenum)A(1), AF(2)); }
void gl_TexEnvx(CPU *c) { glTexEnvf((GLenum)A(0), (GLenum)A(1), fx(A(2))); }
void gl_Flush(CPU *c) { glFlush(); }

void gl_GetBooleanv(CPU *c) { if (A(1)) glGetBooleanv((GLenum)A(0), (GLboolean *)G(A(1))); }
void gl_GetIntegerv(CPU *c) { if (A(1)) glGetIntegerv((GLenum)A(0), (GLint *)G(A(1))); }
void gl_IsEnabled(CPU *c)   { c->r[0] = (uint32_t)glIsEnabled((GLenum)A(0)); }

void gl_ReadPixels(CPU *c)
{
    glReadPixels((GLint)A(0), (GLint)A(1), (GLsizei)A(2), (GLsizei)A(3),
                 (GLenum)A(4), (GLenum)A(5), A(6) ? G(A(6)) : NULL);
}

/* ---- vertex buffer objects ------------------------------------------- */

void gl_GenBuffers(CPU *c)
{
    RESOLVE(pGenBuffers, PFNGLGENBUFFERS, "glGenBuffers");
    if (pGenBuffers) pGenBuffers((GLsizei)A(0), (GLuint *)G(A(1)));
}
void gl_DeleteBuffers(CPU *c)
{
    RESOLVE(pDeleteBuffers, PFNGLDELETEBUFFERS, "glDeleteBuffers");
    if (pDeleteBuffers) pDeleteBuffers((GLsizei)A(0), (const GLuint *)G(A(1)));
}
void gl_BufferData(CPU *c)
{
    RESOLVE(pBufferData, PFNGLBUFFERDATA, "glBufferData");
    if (pBufferData)
        pBufferData((GLenum)A(0), (GLsizeiptr)A(1), A(2) ? G(A(2)) : NULL, (GLenum)A(3));
}
void gl_BufferSubData(CPU *c)
{
    RESOLVE(pBufferSubData, PFNGLBUFFERSUBDATA, "glBufferSubData");
    if (pBufferSubData)
        pBufferSubData((GLenum)A(0), (GLintptr)A(1), (GLsizeiptr)A(2),
                       A(3) ? G(A(3)) : NULL);
}

/* ---- framebuffer objects (OES == EXT token values) ------------------- */

void gl_GenFramebuffersOES(CPU *c)
{
    RESOLVE(pGenFramebuffers, PFNGLGENFRAMEBUFFERSEXT, "glGenFramebuffersEXT");
    if (pGenFramebuffers) pGenFramebuffers((GLsizei)A(0), (GLuint *)G(A(1)));
}
void gl_DeleteFramebuffersOES(CPU *c)
{
    RESOLVE(pDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSEXT, "glDeleteFramebuffersEXT");
    if (pDeleteFramebuffers) pDeleteFramebuffers((GLsizei)A(0), (const GLuint *)G(A(1)));
}
void gl_BindFramebufferOES(CPU *c)
{
    if (getenv("A5_GLDBG")) {
        static unsigned lt = 0xffffffffu, li = 0xffffffffu; static int n = 0;
        unsigned t = (unsigned)A(0), i = (unsigned)A(1);
        if (n < 80 && (t != lt || i != li)) {
            fprintf(stderr, "[gldbg] BindFramebuffer target=%#x id=%u\n", t, i);
            lt = t; li = i; n++;
        }
    }
    RESOLVE(pBindFramebuffer, PFNGLBINDFRAMEBUFFEREXT, "glBindFramebufferEXT");
    if (pBindFramebuffer) pBindFramebuffer((GLenum)A(0), (GLuint)A(1));
}
void gl_CheckFramebufferStatusOES(CPU *c)
{
    RESOLVE(pCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSEXT, "glCheckFramebufferStatusEXT");
    c->r[0] = pCheckFramebufferStatus ? (uint32_t)pCheckFramebufferStatus((GLenum)A(0)) : 0x8CD5u;
}
void gl_GenRenderbuffersOES(CPU *c)
{
    RESOLVE(pGenRenderbuffers, PFNGLGENRENDERBUFFERSEXT, "glGenRenderbuffersEXT");
    if (pGenRenderbuffers) pGenRenderbuffers((GLsizei)A(0), (GLuint *)G(A(1)));
}
void gl_DeleteRenderbuffersOES(CPU *c)
{
    RESOLVE(pDeleteRenderbuffers, PFNGLDELETERENDERBUFFERSEXT, "glDeleteRenderbuffersEXT");
    if (pDeleteRenderbuffers) pDeleteRenderbuffers((GLsizei)A(0), (const GLuint *)G(A(1)));
}
void gl_BindRenderbufferOES(CPU *c)
{
    RESOLVE(pBindRenderbuffer, PFNGLBINDRENDERBUFFEREXT, "glBindRenderbufferEXT");
    if (pBindRenderbuffer) pBindRenderbuffer((GLenum)A(0), (GLuint)A(1));
}
void gl_RenderbufferStorageOES(CPU *c)
{
    if (getenv("A5_GLDBG"))
        fprintf(stderr, "[gldbg] RenderbufferStorage target=%#x fmt=%#x %dx%d\n",
                (unsigned)A(0), (unsigned)A(1), (int)A(2), (int)A(3));
    RESOLVE(pRenderbufferStorage, PFNGLRENDERBUFFERSTORAGEEXT, "glRenderbufferStorageEXT");
    if (pRenderbufferStorage)
        pRenderbufferStorage((GLenum)A(0), (GLenum)A(1), (GLsizei)A(2), (GLsizei)A(3));
}
void gl_FramebufferRenderbufferOES(CPU *c)
{
    RESOLVE(pFramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFEREXT, "glFramebufferRenderbufferEXT");
    if (pFramebufferRenderbuffer)
        pFramebufferRenderbuffer((GLenum)A(0), (GLenum)A(1), (GLenum)A(2), (GLuint)A(3));
}
void gl_FramebufferTexture2DOES(CPU *c)
{
    RESOLVE(pFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DEXT, "glFramebufferTexture2DEXT");
    if (pFramebufferTexture2D)
        pFramebufferTexture2D((GLenum)A(0), (GLenum)A(1), (GLenum)A(2),
                              (GLuint)A(3), (GLint)A(4));
}
