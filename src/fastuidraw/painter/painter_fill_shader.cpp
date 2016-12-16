/*!
 * \file painter_stroke_shader.cpp
 * \brief file painter_stroke_shader.cpp
 *
 * Copyright 2016 by Intel.
 *
 * Contact: kevin.rogovin@intel.com
 *
 * This Source Code Form is subject to the
 * terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with
 * this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * \author Kevin Rogovin <kevin.rogovin@intel.com>
 *
 */

#include <fastuidraw/painter/painter_fill_shader.hpp>

namespace
{
  class PainterFillShaderPrivate
  {
  public:
    fastuidraw::reference_counted_ptr<fastuidraw::PainterItemShader> m_aa_shader;
    fastuidraw::reference_counted_ptr<fastuidraw::PainterItemShader> m_non_aa_shader;
  };
}

//////////////////////////////////////////
// fastuidraw::PainterFillShader methods
fastuidraw::PainterFillShader::
PainterFillShader(void)
{
  m_d = FASTUIDRAWnew PainterFillShaderPrivate();
}

fastuidraw::PainterFillShader::
PainterFillShader(const PainterFillShader &obj)
{
  PainterFillShaderPrivate *d;
  d = reinterpret_cast<PainterFillShaderPrivate*>(obj.m_d);
  m_d = FASTUIDRAWnew PainterFillShaderPrivate(*d);
}

fastuidraw::PainterFillShader::
~PainterFillShader()
{
  PainterFillShaderPrivate *d;
  d = reinterpret_cast<PainterFillShaderPrivate*>(m_d);
  FASTUIDRAWdelete(d);
  m_d = NULL;
}

fastuidraw::PainterFillShader&
fastuidraw::PainterFillShader::
operator=(const PainterFillShader &rhs)
{
  if(this != &rhs)
    {
      PainterFillShaderPrivate *d, *rhs_d;
      d = reinterpret_cast<PainterFillShaderPrivate*>(m_d);
      rhs_d = reinterpret_cast<PainterFillShaderPrivate*>(rhs.m_d);
      *d = *rhs_d;
    }
  return *this;
}


const fastuidraw::reference_counted_ptr<fastuidraw::PainterItemShader>&
fastuidraw::PainterFillShader::
shader(bool with_aa) const
{
  return with_aa ?
    aa_shader() :
    non_aa_shader();
}


#define setget_implement(type, name)                                \
  fastuidraw::PainterFillShader&                                    \
  fastuidraw::PainterFillShader::                                   \
  name(type v)                                                      \
  {                                                                 \
    PainterFillShaderPrivate *d;                                    \
    d = reinterpret_cast<PainterFillShaderPrivate*>(m_d);           \
    d->m_##name = v;                                                \
    return *this;                                                   \
  }                                                                 \
                                                                    \
  type                                                              \
  fastuidraw::PainterFillShader::                                   \
  name(void) const                                                  \
  {                                                                 \
    PainterFillShaderPrivate *d;                                    \
    d = reinterpret_cast<PainterFillShaderPrivate*>(m_d);           \
    return d->m_##name;                                             \
  }

setget_implement(const fastuidraw::reference_counted_ptr<fastuidraw::PainterItemShader>&, aa_shader)
setget_implement(const fastuidraw::reference_counted_ptr<fastuidraw::PainterItemShader>&, non_aa_shader)
#undef setget_implement
