// GeneralsX @feature visionOS port - GL entry-point symbol names of this module.
//
// gles_dispatch.cpp defines a wrapper for every gl* function the backend calls; the
// wrapper forwards to a pointer resolved at runtime (see gles_dispatch.h). On Android
// those wrappers are the global gl* symbols themselves (unchanged behaviour). Everywhere
// else the host links its own GL implementation -- ANGLE on visionOS, which exports the
// standard gl* names -- so the wrappers are renamed d3d8gles_gl* to make a symbol
// collision or an accidental self-recursive dlsym() impossible.
//
// Include BEFORE <GLES3/gl3.h>. Every backend translation unit reaches this header
// through gles_pipeline.h or gles_dispatch.cpp; it is deliberately not part of the
// public include directory contract.
#pragma once
#if !defined(__ANDROID__)
#define glActiveTexture d3d8gles_glActiveTexture
#define glAttachShader d3d8gles_glAttachShader
#define glBindBuffer d3d8gles_glBindBuffer
#define glBindBufferBase d3d8gles_glBindBufferBase
#define glBindFramebuffer d3d8gles_glBindFramebuffer
#define glBindRenderbuffer d3d8gles_glBindRenderbuffer
#define glBindTexture d3d8gles_glBindTexture
#define glBindVertexArray d3d8gles_glBindVertexArray
#define glBlendFunc d3d8gles_glBlendFunc
#define glBlendFuncSeparate d3d8gles_glBlendFuncSeparate
#define glDrawBuffers d3d8gles_glDrawBuffers
#define glClearBufferfv d3d8gles_glClearBufferfv
#define glBlitFramebuffer d3d8gles_glBlitFramebuffer
#define glBufferData d3d8gles_glBufferData
#define glBufferSubData d3d8gles_glBufferSubData
#define glMapBufferRange d3d8gles_glMapBufferRange
#define glUnmapBuffer d3d8gles_glUnmapBuffer
#define glCheckFramebufferStatus d3d8gles_glCheckFramebufferStatus
#define glClear d3d8gles_glClear
#define glClearColor d3d8gles_glClearColor
#define glClearDepthf d3d8gles_glClearDepthf
#define glClearStencil d3d8gles_glClearStencil
#define glColorMask d3d8gles_glColorMask
#define glCompileShader d3d8gles_glCompileShader
#define glCompressedTexImage2D d3d8gles_glCompressedTexImage2D
#define glCreateProgram d3d8gles_glCreateProgram
#define glCreateShader d3d8gles_glCreateShader
#define glCullFace d3d8gles_glCullFace
#define glDeleteBuffers d3d8gles_glDeleteBuffers
#define glDeleteFramebuffers d3d8gles_glDeleteFramebuffers
#define glDeleteProgram d3d8gles_glDeleteProgram
#define glDeleteRenderbuffers d3d8gles_glDeleteRenderbuffers
#define glDeleteShader d3d8gles_glDeleteShader
#define glDeleteTextures d3d8gles_glDeleteTextures
#define glDeleteVertexArrays d3d8gles_glDeleteVertexArrays
#define glDepthFunc d3d8gles_glDepthFunc
#define glDepthMask d3d8gles_glDepthMask
#define glDepthRangef d3d8gles_glDepthRangef
#define glDisable d3d8gles_glDisable
#define glDisableVertexAttribArray d3d8gles_glDisableVertexAttribArray
#define glDrawArrays d3d8gles_glDrawArrays
#define glDrawElements d3d8gles_glDrawElements
#define glEnable d3d8gles_glEnable
#define glEnableVertexAttribArray d3d8gles_glEnableVertexAttribArray
#define glFinish d3d8gles_glFinish
#define glFramebufferRenderbuffer d3d8gles_glFramebufferRenderbuffer
#define glFramebufferTexture2D d3d8gles_glFramebufferTexture2D
#define glGenBuffers d3d8gles_glGenBuffers
#define glGenFramebuffers d3d8gles_glGenFramebuffers
#define glGenRenderbuffers d3d8gles_glGenRenderbuffers
#define glGenTextures d3d8gles_glGenTextures
#define glGenVertexArrays d3d8gles_glGenVertexArrays
#define glGenerateMipmap d3d8gles_glGenerateMipmap
#define glGetError d3d8gles_glGetError
#define glGetIntegerv d3d8gles_glGetIntegerv
#define glGetBooleanv d3d8gles_glGetBooleanv
#define glGenQueries d3d8gles_glGenQueries
#define glBeginQuery d3d8gles_glBeginQuery
#define glEndQuery d3d8gles_glEndQuery
#define glGetQueryObjectuiv d3d8gles_glGetQueryObjectuiv
#define glGetProgramInfoLog d3d8gles_glGetProgramInfoLog
#define glGetProgramiv d3d8gles_glGetProgramiv
#define glGetShaderInfoLog d3d8gles_glGetShaderInfoLog
#define glGetShaderiv d3d8gles_glGetShaderiv
#define glGetString d3d8gles_glGetString
#define glGetUniformBlockIndex d3d8gles_glGetUniformBlockIndex
#define glGetUniformLocation d3d8gles_glGetUniformLocation
#define glLinkProgram d3d8gles_glLinkProgram
#define glPixelStorei d3d8gles_glPixelStorei
#define glReadBuffer d3d8gles_glReadBuffer
#define glReadPixels d3d8gles_glReadPixels
#define glPolygonOffset d3d8gles_glPolygonOffset
#define glRenderbufferStorage d3d8gles_glRenderbufferStorage
#define glScissor d3d8gles_glScissor
#define glShaderSource d3d8gles_glShaderSource
#define glStencilFunc d3d8gles_glStencilFunc
#define glStencilMask d3d8gles_glStencilMask
#define glStencilOp d3d8gles_glStencilOp
#define glTexImage2D d3d8gles_glTexImage2D
#define glTexParameteri d3d8gles_glTexParameteri
#define glUniformBlockBinding d3d8gles_glUniformBlockBinding
#define glUniform1f d3d8gles_glUniform1f
#define glUniform1i d3d8gles_glUniform1i
#define glUniform1iv d3d8gles_glUniform1iv
#define glUniform2f d3d8gles_glUniform2f
#define glUniform3fv d3d8gles_glUniform3fv
#define glUniform4f d3d8gles_glUniform4f
#define glUniform4fv d3d8gles_glUniform4fv
#define glUniformMatrix4fv d3d8gles_glUniformMatrix4fv
#define glUseProgram d3d8gles_glUseProgram
#define glVertexAttribPointer d3d8gles_glVertexAttribPointer
#define glViewport d3d8gles_glViewport
#endif
