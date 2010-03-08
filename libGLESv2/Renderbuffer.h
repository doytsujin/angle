//
// Copyright (c) 2002-2010 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// Renderbuffer.h: Defines the virtual gl::Renderbuffer class and its derived
// classes Colorbuffer, Depthbuffer and Stencilbuffer. Implements GL renderbuffer
// objects and related functionality. [OpenGL ES 2.0.24] section 4.4.3 page 108.

#ifndef LIBGLESV2_RENDERBUFFER_H_
#define LIBGLESV2_RENDERBUFFER_H_

#define GL_APICALL
#include <GLES2/gl2.h>
#include <d3d9.h>

#include "angleutils.h"

namespace gl
{
class Renderbuffer
{
  public:
    Renderbuffer();

    virtual ~Renderbuffer();

    virtual bool isColorbuffer();
    virtual bool isDepthbuffer();
    virtual bool isStencilbuffer();

    virtual IDirect3DSurface9 *getRenderTarget();
    virtual IDirect3DSurface9 *getDepthStencil();

    int getWidth();
    int getHeight();

  protected:
    int mWidth;
    int mHeight;

  private:
    DISALLOW_COPY_AND_ASSIGN(Renderbuffer);
};

class Colorbuffer : public Renderbuffer
{
  public:
    Colorbuffer(IDirect3DSurface9 *renderTarget);

    ~Colorbuffer();

    bool isColorbuffer();

    GLuint getRedSize();
    GLuint getGreenSize();
    GLuint getBlueSize();
    GLuint getAlphaSize();

    IDirect3DSurface9 *getRenderTarget();

  protected:
    IDirect3DSurface9 *mRenderTarget;

  private:
    DISALLOW_COPY_AND_ASSIGN(Colorbuffer);
};

class Depthbuffer : public Renderbuffer
{
  public:
    Depthbuffer(IDirect3DSurface9 *depthStencil);
    Depthbuffer(int width, int height);

    ~Depthbuffer();

    bool isDepthbuffer();

    GLuint getDepthSize();

    IDirect3DSurface9 *getDepthStencil();

  private:
    DISALLOW_COPY_AND_ASSIGN(Depthbuffer);
    IDirect3DSurface9 *mDepthStencil;
};

class Stencilbuffer : public Renderbuffer
{
  public:
    Stencilbuffer(IDirect3DSurface9 *depthStencil);

    ~Stencilbuffer();

    bool isStencilbuffer();

    GLuint getStencilSize();

    IDirect3DSurface9 *getDepthStencil();

  private:
    DISALLOW_COPY_AND_ASSIGN(Stencilbuffer);
    IDirect3DSurface9 *mDepthStencil;
};
}

#endif   // LIBGLESV2_RENDERBUFFER_H_
