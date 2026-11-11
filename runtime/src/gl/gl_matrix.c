/* Matrix stack entry points.
 *
 * Split out of the original monolithic gles1.c during the
 * 2026-10 project reorganisation.  Shared state lives in gl_globals.c
 * and is declared in gl_internal.h.
 */

#include "gl_internal.h"
#include "platform/kbm.h"


void gl_Translatef(CPU *c) { glTranslatef(AF(0), AF(1), AF(2)); }


void gl_Rotatef(CPU *c) { glRotatef(AF(0), AF(1), AF(2), AF(3)); }


void gl_LoadMatrixf(CPU *c)
{
    const GLfloat *m = (const GLfloat *)AP(0);
    if (g_verbose && g_state_log < 6) {
        fprintf(stderr, "[gl] LoadMatrixf [%d] %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f\n",
                g_state_log, m?m[0]:0, m?m[1]:0, m?m[2]:0, m?m[3]:0,
                m?m[4]:0, m?m[5]:0, m?m[6]:0, m?m[7]:0,
                m?m[8]:0, m?m[9]:0, m?m[10]:0, m?m[11]:0,
                m?m[12]:0, m?m[13]:0, m?m[14]:0, m?m[15]:0);
        g_state_log++;
    }
    if (gl_stats_env() && g_swap_count > 400 && g_mtx_probe < 12) {
        GLint mode = 0;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        fprintf(stderr, "[mtx] lr=%x mode=%x m=%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
                c->r[14], (unsigned)mode,
                m?m[0]:0, m?m[1]:0, m?m[2]:0, m?m[3]:0,
                m?m[4]:0, m?m[5]:0, m?m[6]:0, m?m[7]:0,
                m?m[8]:0, m?m[9]:0, m?m[10]:0, m?m[11]:0,
                m?m[12]:0, m?m[13]:0, m?m[14]:0, m?m[15]:0);
        g_mtx_probe++;
    }
    if (getenv("A5_GLDBG")) {
        static float last[16];
        static int n = 0;
        GLint mode = 0;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        if (m && mode == GL_PROJECTION && n < 40) {
            int diff = 0;
            for (int i = 0; i < 16; i++)
                if (fabsf(m[i] - last[i]) > 1e-4f) diff = 1;
            if (diff) {
                fprintf(stderr, "[gldbg] Proj [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                        "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
                        m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13],
                        m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]);
                for (int i = 0; i < 16; i++) last[i] = m[i];
                n++;
            }
        }
    }
    if (getenv("RECOMP_GLDBG_PROJ")) {
        static int n;
        GLint mode = 0;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        if (m && mode == GL_PROJECTION && n < 200) {
            fprintf(stderr, "[proj] frame=%d %d m0=%.4f m5=%.4f m11=%.3f m15=%.3f\n",
                    (int)g_swap_count, n, m[0], m[5], m[11], m[15]);
            n++;
        }
    }
    /* Scope zoom (PC input mode): narrow the 3D perspective only.  The HUD is
     * drawn with an orthographic projection (m[15] == 1), so it keeps its
     * size and the touch widgets stay where the player sees them. */
    if (m) {
        float z = kbm_zoom_scale();
        if (z > 1.0001f && fabsf(m[15]) < 1e-6f && fabsf(m[11]) > 0.5f) {
            GLint mode = 0;
            glGetIntegerv(GL_MATRIX_MODE, &mode);
            if (mode == GL_PROJECTION) {
                GLfloat p[16];
                memcpy(p, m, sizeof(p));
                p[0] /= z;
                p[5] /= z;
                glLoadMatrixf(p);
                return;
            }
        }
    }
    glLoadMatrixf(m);
}


void gl_MatrixMode(CPU *c) { glMatrixMode((GLenum)A(0)); }


void gl_LoadIdentity(CPU *c) { glLoadIdentity(); }


void gl_PushMatrix(CPU *c) { glPushMatrix(); }


void gl_PopMatrix(CPU *c) { glPopMatrix(); }
