#!/usr/bin/env python3
"""Generate OpenGL import shims (guest i386 cdecl -> host OpenGL) from a signature table.

Arg codes:  e enum/uint   i int   u uint   f float (1 slot)   d double (2 slots)
            p input pointer (NULL stays NULL)   o output pointer
            B array-buffer-relative pointer (offset when a VBO is bound, else guest pointer)
            X element-buffer-relative pointer
Return:     v void   u uint   i int
"""
import sys

SPEC = """
glActiveTexture v e
glAlphaFunc v e f
glAttachShader v u u
glBindAttribLocation v u u p
glBindBuffer v e u
glBindFramebufferEXT v e u
glBindProgramARB v e u
glBindRenderbufferEXT v e u
glBindTexture v e u
glBindVertexArrayAPPLE v u
glBlendEquation v e
glBlendEquationSeparate v e e
glBlendFunc v e e
glBlendFuncSeparate v e e e e
glClear v u
glClearColor v f f f f
glClearDepth v d
glClientActiveTexture v e
glColor4f v f f f f
glColorMask v u u u u
glColorPointer v i e i B
glCompressedTexImage2D v e i e i i i i p
glCreateProgram u
glCreateShader u e
glCullFace v e
glDeleteBuffers v i p
glDeleteFramebuffersEXT v i p
glDeleteProgram v u
glDeleteProgramsARB v i p
glDeleteRenderbuffersEXT v i p
glDeleteShader v u
glDeleteTextures v i p
glDeleteVertexArraysAPPLE v i p
glDepthFunc v e
glDepthMask v u
glDepthRange v d d
glDetachShader v u u
glDisable v e
glDisableClientState v e
glDisableVertexAttribArray v u
glDrawArrays v e i i
glDrawRangeElements v e u u i e X
glEnable v e
glEnableClientState v e
glEnableVertexAttribArray v u
glFlush v
glFramebufferRenderbufferEXT v e e e u
glFramebufferTexture2DEXT v e e e u i
glFrontFace v e
glGenBuffers v i o
glGenFramebuffersEXT v i o
glGenProgramsARB v i o
glGenRenderbuffersEXT v i o
glGenTextures v i o
glGenVertexArraysAPPLE v i o
glGetError u
glGetProgramInfoLog v u i o o
glGetProgramiv v u e o

glGetShaderInfoLog v u i o o
glGetShaderSource v u i o o
glGetShaderiv v u e o
glGetTexImage v e i e e o
glGetTexLevelParameteriv v e i e o
glGetUniformLocation i u p
glLightModelf v e f
glLightModelfv v e p
glLightf v e e f
glLightfv v e e p
glLoadIdentity v
glLoadMatrixf v p
glMaterialf v e e f
glMaterialfv v e e p
glMatrixMode v e
glNormal3f v f f f
glNormalPointer v e i B
glPixelStorei v e i
glPolygonOffset v f f
glProgramEnvParameters4fvEXT v e u i p
glProgramStringARB v e e i p
glReadPixels v i i i i e e o
glRenderbufferStorageEXT v e e i i
glStencilFunc v e i u
glStencilMask v u
glStencilOp v e e e
glTexCoordPointer v i e i B
glTexEnvfv v e e p
glTexEnvi v e e i
glTexImage2D v e i i i i i e e p
glTexParameterf v e e f
glTexParameterfv v e e p
glTexParameteri v e e i
glTexSubImage2D v e i i i i i e e p
glUniform1f v i f
glUniform1i v i i
glUniform4fv v i i p
glUniformMatrix4fv v i i u p
glUseProgram v u
glVertexAttrib1fv v u p
glVertexAttrib2fv v u p
glVertexAttrib3fv v u p
glVertexAttrib4fv v u p
glVertexAttribPointer v u i e u i B
glVertexPointer v i e i B
"""

# guestname hostname ret args   (guest calls the name on the left; host function on the right)
# Claude Fable - 2026-11-24: glBlitFramebuffer and glBlitFramebufferEXT were removed from this table. They are hand-written in
# runtime/gl.c now, because copies to the window must be mapped from the game's resolution to the real window (and
# may be handed to MetalFX). Do not add them back here: the linker would report a duplicate symbol.
ALIAS = """
glBindBufferBase glBindBufferBase v e u u
glBindSampler glBindSampler v u u
glClearBufferfi glClearBufferfi v e i f i
glClearBufferfv glClearBufferfv v e i p
glClearBufferiv glClearBufferiv v e i p
glColorMaski glColorMaski v u u u u u
glDeleteSamplers glDeleteSamplers v i p
glGenSamplers glGenSamplers v i o
glSamplerParameterf glSamplerParameterf v u e f
glSamplerParameterfv glSamplerParameterfv v u e p
glSamplerParameteri glSamplerParameteri v u e i
glDisablei glDisablei v e u
glEnablei glEnablei v e u
glDrawElementsBaseVertex glDrawElementsBaseVertex v e i e X i
glDrawElementsInstancedBaseVertex glDrawElementsInstancedBaseVertex v e i e X i i
glDrawRangeElementsBaseVertex glDrawRangeElementsBaseVertex v e u u i e X i
glGetUniformBlockIndex glGetUniformBlockIndex u u p
glUniformBlockBinding glUniformBlockBinding v u u u
glSampleMaski glSampleMaski v u u
glTexImage2DMultisample glTexImage2DMultisample v e i e i i u
glTexBuffer glTexBuffer v e e u
glActiveTextureARB glActiveTexture v e
glBindBufferARB glBindBuffer v e u
glDeleteBuffersARB glDeleteBuffers v i p
glGenBuffersARB glGenBuffers v i o
glDisableVertexAttribArrayARB glDisableVertexAttribArray v u
glEnableVertexAttribArrayARB glEnableVertexAttribArray v u
glVertexAttribPointerARB glVertexAttribPointer v u i e u i B
glBindFramebuffer glBindFramebuffer v e u
glBindRenderbuffer glBindRenderbuffer v e u
glDeleteFramebuffers glDeleteFramebuffers v i p
glDeleteRenderbuffers glDeleteRenderbuffers v i p
glGenFramebuffers glGenFramebuffers v i o
glGenRenderbuffers glGenRenderbuffers v i o
glFramebufferRenderbuffer glFramebufferRenderbuffer v e e e u
glFramebufferTexture2D glFramebufferTexture2D v e e e u i
glFramebufferTextureLayer glFramebufferTextureLayer v e e u i i
glRenderbufferStorage glRenderbufferStorage v e e i i
glRenderbufferStorageMultisample glRenderbufferStorageMultisample v e i e i i
glRenderbufferStorageMultisampleEXT glRenderbufferStorageMultisample v e i e i i
glGenerateMipmap glGenerateMipmap v e
glBindVertexArray glBindVertexArray v u
glDeleteVertexArrays glDeleteVertexArrays v i p
glGenVertexArrays glGenVertexArrays v i o
glBlendEquationSeparateEXT glBlendEquationSeparate v e e
glBlendFuncSeparateEXT glBlendFuncSeparate v e e e e
glBlendColor glBlendColor v f f f f
glClearStencil glClearStencil v i
glClipPlane glClipPlane v e p
glColorMaterial glColorMaterial v e e
glCompressedTexImage3D glCompressedTexImage3D v e i e i i i i i p
glCompressedTexImage3DARB glCompressedTexImage3D v e i e i i i i i p
glCompressedTexSubImage2D glCompressedTexSubImage2D v e i i i i i e i p
glCompressedTexSubImage3D glCompressedTexSubImage3D v e i i i i i i i e i p
glDrawBuffer glDrawBuffer v e
glDrawBuffers glDrawBuffers v i p
glDrawBuffersARB glDrawBuffers v i p
glReadBuffer glReadBuffer v e
glDrawArraysInstancedARB glDrawArraysInstanced v e i i i
glDrawElementsInstancedARB glDrawElementsInstanced v e i e X i
glVertexAttribDivisorARB glVertexAttribDivisor v u u
glFinish glFinish v
glFogf glFogf v e f
glGetAttribLocation glGetAttribLocation i u p
glGetFloatv glGetFloatv v e o
glGetTexParameterfv glGetTexParameterfv v e e o
glGetTexParameteriv glGetTexParameteriv v e e o
glHint glHint v e e
glLightModeli glLightModeli v e i
glPolygonMode glPolygonMode v e e
glPopMatrix glPopMatrix v
glPushMatrix glPushMatrix v
glShadeModel glShadeModel v e
glStencilFuncSeparate glStencilFuncSeparate v e e i u
glStencilFuncSeparateATI glStencilFuncSeparateATI v e e i u
glStencilOpSeparate glStencilOpSeparate v e e e e
glStencilOpSeparateATI glStencilOpSeparateATI v e e e e
glActiveStencilFaceEXT glActiveStencilFaceEXT v e
glTexCoord2f glTexCoord2f v f f
glVertex2f glVertex2f v f f
glBegin glBegin v e
glEnd glEnd v
glTexEnvf glTexEnvf v e e f
glTexImage3D glTexImage3D v e i i i i i i e e p
glTexSubImage3D glTexSubImage3D v e i i i i i i i e e p
glTexParameteriv glTexParameteriv v e e p
glUniform1fv glUniform1fv v i i p
glUniform1iv glUniform1iv v i i p
glUniform2fv glUniform2fv v i i p
glUniform2iv glUniform2iv v i i p
glUniform3fv glUniform3fv v i i p
glUniform3iv glUniform3iv v i i p
glUniform4iv glUniform4iv v i i p
glUniformMatrix2fv glUniformMatrix2fv v i i u p
glUniformMatrix3fv glUniformMatrix3fv v i i u p
glValidateProgram glValidateProgram v u
glBeginQueryARB glBeginQuery v e u
glEndQueryARB glEndQuery v e
glGenQueriesARB glGenQueries v i o
glDeleteQueriesARB glDeleteQueries v i p
glGetQueryObjectuivARB glGetQueryObjectuiv v u e o
glGetCompressedTexImageARB glGetCompressedTexImage v e i o
glClampColorARB glClampColor v e e
glBufferParameteriAPPLE glBufferParameteriAPPLE v e e i
glGenFencesAPPLE glGenFencesAPPLE v i o
glDeleteFencesAPPLE glDeleteFencesAPPLE v i p
glSetFenceAPPLE glSetFenceAPPLE v u
glTestFenceAPPLE glTestFenceAPPLE u u
glFinishObjectAPPLE glFinishObjectAPPLE v e i
glBindBufferBaseEXT glBindBufferBase v e u u
glVertexAttribIPointer glVertexAttribIPointer v u i e i B
glBindFragDataLocation glBindFragDataLocation v u u p
"""

# GL 3+ only: not available on the host's GL 2.1 context. Logged once, do nothing, return 0.
# ⚠ if the engine really calls one of these it expects a result, and the stub's 0 may mislead it.
STUBS = """
glTransformFeedbackVaryingsEXT glUniformBufferEXT glGetUniformBufferSizeEXT
glMapBufferARB glUnmapBufferARB
"""


def gen(out):
    lines = ['// GENERATED by tools/glgen.py - do not edit', '#define GL_SILENCE_DEPRECATION',
             '#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED', '#include "rt.h"', '#include <OpenGL/gl.h>', '#include <OpenGL/glext.h>', '#include <OpenGL/gl3.h>',
             'void *gl_ptr_array(uint32_t g);', 'void *gl_ptr_element(uint32_t g);',
             'void gl_trace(const char *name);',
             'void gl_trace_args(const char *name, CPU *c, int nslots);', 'void gl_enter(CPU *c); void gl_leave(void);',
             '#define P(g) ((g) ? G2H(g) : NULL)', '']
    for line in SPEC.strip().splitlines():
        if not line.strip(): continue
        name, ret, *args = line.split()
        slot, cargs = 0, []
        for a in args:
            if a in "eu": cargs.append("(GLuint)ARG(%d)" % slot); slot += 1
            elif a == "i": cargs.append("(GLint)ARG(%d)" % slot); slot += 1
            elif a == "f": cargs.append("ARGF(%d)" % slot); slot += 1
            elif a == "d": cargs.append("ARGD(%d)" % slot); slot += 2
            elif a in "po": cargs.append("P(ARG(%d))" % slot); slot += 1
            elif a == "B": cargs.append("gl_ptr_array(ARG(%d))" % slot); slot += 1
            elif a == "X": cargs.append("gl_ptr_element(ARG(%d))" % slot); slot += 1
            else: raise SystemExit("bad code " + a)
        call = "%s(%s)" % (name, ", ".join(cargs))
        body = call + ";" if ret == "v" else "c->eax = (uint32_t)%s;" % call
        lines.append('void I__%s(CPU *c) { gl_trace("%s"); gl_trace_args("%s", c, %d); gl_enter(c); %s gl_leave(); }' % (name, name, name, slot, body))
    for line in ALIAS.strip().splitlines():
        if not line.strip(): continue
        name, host, ret, *args = line.split()
        slot, cargs = 0, []
        for a in args:
            if a in "eu": cargs.append("(GLuint)ARG(%d)" % slot); slot += 1
            elif a == "i": cargs.append("(GLint)ARG(%d)" % slot); slot += 1
            elif a == "f": cargs.append("ARGF(%d)" % slot); slot += 1
            elif a in "po": cargs.append("P(ARG(%d))" % slot); slot += 1
            elif a == "B": cargs.append("gl_ptr_array(ARG(%d))" % slot); slot += 1
            elif a == "X": cargs.append("gl_ptr_element(ARG(%d))" % slot); slot += 1
            else: raise SystemExit("bad code " + a)
        call = "%s(%s)" % (host, ", ".join(cargs))
        body = call + ";" if ret == "v" else "c->eax = (uint32_t)%s;" % call
        lines.append('void I__%s(CPU *c) { gl_trace("%s"); gl_trace_args("%s", c, %d); gl_enter(c); %s gl_leave(); }' % (name, name, name, slot, body))
    for name in " ".join(STUBS.split("\n")).split():
        if name in ("glMapBufferARB", "glUnmapBufferARB"): continue
        lines.append('void I__%s(CPU *c) { static int once; if (!once++) fprintf(stderr, "[gl] stub (not available in a core context, does nothing): %s\\n"); c->eax = 0; }' % (name, name))
    lines.insert(1, '#include <stdio.h>')
    open(out, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    gen(sys.argv[1])
