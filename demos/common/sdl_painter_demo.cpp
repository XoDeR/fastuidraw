#include "sdl_painter_demo.hpp"
#include "text_helper.hpp"

namespace
{
  const char*
  string_from_data_store_type(enum fastuidraw::gl::PainterBackendGL::data_store_backing_t v)
  {
    switch(v)
      {
      case fastuidraw::gl::PainterBackendGL::data_store_tbo:
        return "tbo";

      case fastuidraw::gl::PainterBackendGL::data_store_ubo:
        return "ubo";
      }

    return "invalid value";
  }

  std::ostream&
  operator<<(std::ostream &ostr, const fastuidraw::PainterShader::Tag &tag)
  {
    ostr << "(ID=" << tag.m_ID << ", group=" << tag.m_group << ")";
    return ostr;
  }

  void
  print_glyph_shader_ids(const fastuidraw::PainterGlyphShader &sh)
  {
    for(unsigned int i = 0; i < sh.shader_count(); ++i)
      {
        enum fastuidraw::glyph_type tp;
        tp = static_cast<enum fastuidraw::glyph_type>(i);
        std::cout << "\t\t#" << i << ": " << sh.shader(tp)->tag() << "\n";
      }
  }

  void
  print_stroke_shader_ids(const fastuidraw::PainterStrokeShader &sh,
                          const std::string &prefix = "\t\t")
  {
    std::cout << prefix << "aa_shader_pass1: " << sh.aa_shader_pass1()->tag() << "\n"
              << prefix << "aa_shader_pass2: " << sh.aa_shader_pass2()->tag() << "\n"
              << prefix << "non_aa_shader: " << sh.non_aa_shader()->tag() << "\n";
  }

  void
  print_dashed_stroke_shader_ids(const fastuidraw::PainterDashedStrokeShaderSet &sh)
  {
    std::cout << "\t\tflat_caps:\n";
    print_stroke_shader_ids(sh.shader(fastuidraw::PainterEnums::flat_caps), "\t\t\t");

    std::cout << "\t\trounded_caps:\n";
    print_stroke_shader_ids(sh.shader(fastuidraw::PainterEnums::rounded_caps), "\t\t\t");

    std::cout << "\t\tsquare_caps:\n";
    print_stroke_shader_ids(sh.shader(fastuidraw::PainterEnums::square_caps), "\t\t\t");
  }
}

sdl_painter_demo::
sdl_painter_demo(const std::string &about_text,
                 bool default_value_for_print_painter_config):
  sdl_demo(about_text),

  m_image_atlas_options("Image Atlas Options", *this),
  m_log2_color_tile_size(m_image_atlas_params.log2_color_tile_size(), "log2_color_tile_size",
                         "Specifies the log2 of the width and height of each color tile",
                         *this),
  m_log2_num_color_tiles_per_row_per_col(m_image_atlas_params.log2_num_color_tiles_per_row_per_col(),
                                         "log2_num_color_tiles_per_row_per_col",
                                         "Specifies the log2 of the number of color tiles "
                                         "in each row and column of each layer; note that "
                                         "then the total number of color tiles available "
                                         "is given as num_color_layers*pow(2, 2*log2_num_color_tiles_per_row_per_col)",
                                         *this),
  m_num_color_layers(m_image_atlas_params.num_color_layers(), "num_color_layers",
                     "Specifies the number of layers in the color texture; note that "
                     "then the total number of color tiles available "
                     "is given as num_color_layers*pow(2, 2*log2_num_color_tiles_per_row_per_col)",
                     *this),
  m_log2_index_tile_size(m_image_atlas_params.log2_index_tile_size(), "log2_index_tile_size",
                         "Specifies the log2 of the width and height of each index tile",
                         *this),
  m_log2_num_index_tiles_per_row_per_col(m_image_atlas_params.log2_num_index_tiles_per_row_per_col(),
                                         "log2_num_index_tiles_per_row_per_col",
                                         "Specifies the log2 of the number of index tiles "
                                         "in each row and column of each layer; note that "
                                         "then the total number of index tiles available "
                                         "is given as num_index_layers*pow(2, 2*log2_num_index_tiles_per_row_per_col)",
                                         *this),
  m_num_index_layers(m_image_atlas_params.num_index_layers(), "num_index_layers",
                     "Specifies the number of layers in the index texture; note that "
                     "then the total number of index tiles available "
                     "is given as num_index_layers*pow(2, 2*log2_num_index_tiles_per_row_per_col)",
                     *this),
  m_image_atlas_delayed_upload(m_image_atlas_params.delayed(),
                               "image_atlas_delayed_upload",
                               "if true delay uploading of data to GL from image atlas until atlas flush",
                               *this),

  m_glyph_atlas_options("Glyph Atlas options", *this),
  m_texel_store_width(m_glyph_atlas_params.texel_store_dimensions().x(),
                      "texel_store_width", "width of texel store", *this),
  m_texel_store_height(m_glyph_atlas_params.texel_store_dimensions().y(),
                       "texel_store_height", "height of texel store", *this),
  m_texel_store_num_layers(m_glyph_atlas_params.texel_store_dimensions().z(),
                           "texel_store_num_layers", "number of layers of texel store", *this),
  m_geometry_store_size(m_glyph_atlas_params.number_floats(),
                        "geometry_store_size", "size of geometry store in floats", *this),
  m_geometry_store_alignment(m_glyph_atlas_params.alignment(),
                              "geometry_store_alignment",
                              "alignment of the geometry store, must be one of 1, 2, 3 or 4", *this),
  m_glyph_atlas_delayed_upload(m_glyph_atlas_params.delayed(),
                               "glyph_atlas_delayed_upload",
                               "if true delay uploading of data to GL from glyph atlas until atlas flush",
                               *this),
  m_glyph_geometry_backing_store_type(glyph_geometry_backing_store_auto,
                                      enumerated_string_type<enum glyph_geometry_backing_store_t>()
                                      .add_entry("buffer",
                                                 glyph_geometry_backing_store_texture_buffer,
                                                 "use a texture buffer, feature is core in GL but for GLES requires version 3.2, "
                                                 "for GLES version pre-3.2, requires the extension GL_OES_texture_buffer or the "
                                                 "extension GL_EXT_texture_buffer")
                                      .add_entry("texture_array",
                                                 glyph_geometry_backing_store_texture_array,
                                                 "use a 2D texture array to store the glyph geometry data, "
                                                 "GL and GLES have feature in core")
                                      .add_entry("auto",
                                                 glyph_geometry_backing_store_auto,
                                                 "query context and decide optimal value"),
                                      "geometry_backing_store_type",
                                      "Determines how the glyph geometry store is backed.",
                                      *this),
  m_glyph_geometry_backing_texture_log2_w(10, "glyph_geometry_backing_texture_log2_w",
                                          "If glyph_geometry_backing_store_type is set to texture_array, then "
                                          "this gives the log2 of the width of the texture array", *this),
  m_glyph_geometry_backing_texture_log2_h(10, "glyph_geometry_backing_texture_log2_h",
                                          "If glyph_geometry_backing_store_type is set to texture_array, then "
                                          "this gives the log2 of the height of the texture array", *this),
  m_colorstop_atlas_options("ColorStop Atlas options", *this),
  m_color_stop_atlas_width(m_colorstop_atlas_params.width(),
                           "colorstop_atlas_width",
                           "width for color stop atlas", *this),
  m_color_stop_atlas_use_optimal_width(false, "colorstop_atlas_use_optimal_width",
                                       "if true ignore the value of colorstop_atlas_layers "
                                       "and query the GL context for the optimal width for "
                                       "the colorstop atlas",
                                       *this),
  m_color_stop_atlas_layers(m_colorstop_atlas_params.num_layers(),
                            "colorstop_atlas_layers",
                            "number of layers for the color stop atlas",
                            *this),
  m_color_stop_atlas_delayed_upload(m_colorstop_atlas_params.delayed(),
                                    "color_stop_atlas_delayed_upload",
                                    "if true delay uploading of data to GL from color stop atlas until atlas flush",
                                    *this),

  m_painter_options("PainterBackendGL Options", *this),
  m_painter_attributes_per_buffer(m_painter_params.attributes_per_buffer(),
                                  "painter_verts_per_buffer",
                                  "Number of vertices a single API draw can hold",
                                  *this),
  m_painter_indices_per_buffer(m_painter_params.indices_per_buffer(),
                               "painter_indices_per_buffer",
                               "Number of indices a single API draw can hold",
                               *this),
  m_painter_number_pools(m_painter_params.number_pools(), "painter_number_pools",
                         "Number of GL object pools used by the painter", *this),
  m_painter_break_on_shader_change(m_painter_params.break_on_shader_change(),
                                   "painter_break_on_shader_change",
                                   "If true, different shadings are placed into different "
                                   "entries of a call to glMultiDrawElements", *this),
  m_uber_vert_use_switch(m_painter_params.vert_shader_use_switch(),
                         "painter_uber_vert_use_switch",
                         "If true, use a switch statement in uber vertex shader dispatch",
                         *this),
  m_uber_frag_use_switch(m_painter_params.frag_shader_use_switch(),
                         "painter_uber_frag_use_switch",
                         "If true, use a switch statement in uber fragment shader dispatch",
                         *this),
  m_uber_blend_use_switch(m_painter_params.blend_shader_use_switch(),
                          "painter_uber_blend_use_switch",
                          "If true, use a switch statement in uber blend shader dispatch",
                          *this),
  m_unpack_header_and_brush_in_frag_shader(m_painter_params.unpack_header_and_brush_in_frag_shader(),
                                           "painter_unpack_header_and_brush_in_frag_shader",
                                           "if true, unpack the brush and frag-shader specific data from "
                                           "the header in the fragment shader instead of the vertex shader",
                                           *this),
  m_separate_program_for_discard(m_painter_params.separate_program_for_discard(),
                                 "separate_program_for_discard",
                                 "if true, there are two GLSL programs active when drawing: "
                                 "one for those item shaders that have discard and one for "
                                 "those that do not",
                                 *this),
  m_non_dashed_stroke_shader_uses_discard(m_painter_params.non_dashed_stroke_shader_uses_discard(),
                                          "non_dashed_stroke_shader_uses_discard",
                                          "Use discard in instead of thinner widths when stroking "
                                          "opaque pass for anti-aliased stroking of paths",
                                          *this),

  m_painter_options_affected_by_context("PainterBackendGL Options that can be overridden "
                                        "by version and extension supported by GL/GLES context",
                                        *this),
  m_use_hw_clip_planes(m_painter_params.use_hw_clip_planes(),
                       "painter_use_hw_clip_planes",
                       "If true, use HW clip planes (i.e. gl_ClipDistance) for clipping",
                       *this),
  m_painter_alignment(m_painter_base_params.alignment(), "painter_alignment",
                       "Alignment for data store of painter, must be 1, 2, 3 or 4", *this),
  m_painter_data_blocks_per_buffer(m_painter_params.data_blocks_per_store_buffer(),
                                   "painter_blocks_per_buffer",
                                   "Number of data blocks a single API draw can hold",
                                   *this),
  m_data_store_backing(m_painter_params.data_store_backing(),
                       enumerated_string_type<data_store_backing_t>()
                       .add_entry("tbo",
                                  fastuidraw::gl::PainterBackendGL::data_store_tbo,
                                  "use a texture buffer (if available) to back the data store. "
                                  "A texture buffer can have a very large maximum size")
                       .add_entry("ubo",
                                  fastuidraw::gl::PainterBackendGL::data_store_ubo,
                                  "use a uniform buffer object to back the data store. "
                                  "A uniform buffer object's maximum size is much smaller than that "
                                  "of a texture buffer object usually"),
                       "painter_data_store_backing_type",
                       "specifies how the data store buffer is backed",
                       *this),
  m_assign_layout_to_vertex_shader_inputs(m_painter_params.assign_layout_to_vertex_shader_inputs(),
                                          "painter_assign_layout_to_vertex_shader_inputs",
                                          "If true, use layout(location=) in GLSL shader for vertex shader inputs",
                                          *this),
  m_assign_layout_to_varyings(m_painter_params.assign_layout_to_varyings(),
                              "painter_assign_layout_to_varyings",
                              "If true, use layout(location=) in GLSL shader for varyings", *this),
  m_assign_binding_points(m_painter_params.assign_binding_points(),
                          "painter_assign_binding_points",
                          "If true, use layout(binding=) in GLSL shader on samplers and buffers", *this),
  m_use_ubo_for_uniforms(m_painter_params.use_ubo_for_uniforms(),
                         "painter_use_ubo_for_uniforms",
                         "If true, use a UBO instead of uniforms to hold uniform values common to all items",
                         *this),
  m_demo_options("Demo Options", *this),
  m_print_painter_config(default_value_for_print_painter_config, "print_painter_config", "Print PainterBackendGL config", *this)
{}

sdl_painter_demo::
~sdl_painter_demo()
{}

void
sdl_painter_demo::
init_gl(int w, int h)
{
  m_image_atlas_params
    .log2_color_tile_size(m_log2_color_tile_size.m_value)
    .log2_num_color_tiles_per_row_per_col(m_log2_num_color_tiles_per_row_per_col.m_value)
    .num_color_layers(m_num_color_layers.m_value)
    .log2_index_tile_size(m_log2_index_tile_size.m_value)
    .log2_num_index_tiles_per_row_per_col(m_log2_num_index_tiles_per_row_per_col.m_value)
    .num_index_layers(m_num_index_layers.m_value)
    .delayed(m_image_atlas_delayed_upload.m_value);
  m_image_atlas = FASTUIDRAWnew fastuidraw::gl::ImageAtlasGL(m_image_atlas_params);

  fastuidraw::ivec3 texel_dims(m_texel_store_width.m_value, m_texel_store_height.m_value, m_texel_store_num_layers.m_value);
  m_glyph_atlas_params
    .texel_store_dimensions(texel_dims)
    .number_floats(m_geometry_store_size.m_value)
    .alignment(m_geometry_store_alignment.m_value)
    .delayed(m_glyph_atlas_delayed_upload.m_value);

  switch(m_glyph_geometry_backing_store_type.m_value.m_value)
    {
    case glyph_geometry_backing_store_texture_buffer:
      m_glyph_atlas_params.use_texture_buffer_geometry_store();
      break;

    case glyph_geometry_backing_store_texture_array:
      m_glyph_atlas_params.use_texture_2d_array_geometry_store(m_glyph_geometry_backing_texture_log2_w.m_value,
                                                               m_glyph_geometry_backing_texture_log2_h.m_value);
      break;

    default:
      m_glyph_atlas_params.use_optimal_geometry_store_backing();
      switch(m_glyph_atlas_params.glyph_geometry_backing_store_type())
        {
        case fastuidraw::glsl::PainterBackendGLSL::glyph_geometry_tbo:
          {
            std::cout << "Glyph Geometry Store: auto selected buffer\n";
          }
          break;

        case fastuidraw::glsl::PainterBackendGLSL::glyph_geometry_texture_array:
          {
            fastuidraw::ivec2 log2_dims(m_glyph_atlas_params.texture_2d_array_geometry_store_log2_dims());
            std::cout << "Glyph Geometry Store: auto selected texture with dimensions: (2^"
                      << log2_dims.x() << ", 2^" << log2_dims.y() << ") = "
                      << fastuidraw::ivec2(1 << log2_dims.x(), 1 << log2_dims.y())
                      << "\n";
          }
          break;
        }
    }
  m_glyph_atlas = FASTUIDRAWnew fastuidraw::gl::GlyphAtlasGL(m_glyph_atlas_params);

  m_colorstop_atlas_params
    .width(m_color_stop_atlas_width.m_value)
    .num_layers(m_color_stop_atlas_layers.m_value)
    .delayed(m_color_stop_atlas_delayed_upload.m_value);

  if(m_color_stop_atlas_use_optimal_width.m_value)
    {
      m_colorstop_atlas_params.optimal_width();
      std::cout << "Colorstop Atlas optimal width selected to be "
                << m_colorstop_atlas_params.width() << "\n";
    }

  m_colorstop_atlas = FASTUIDRAWnew fastuidraw::gl::ColorStopAtlasGL(m_colorstop_atlas_params);


  m_painter_base_params.alignment(m_painter_alignment.m_value);
  m_painter_params
    .image_atlas(m_image_atlas)
    .glyph_atlas(m_glyph_atlas)
    .colorstop_atlas(m_colorstop_atlas)
    .attributes_per_buffer(m_painter_attributes_per_buffer.m_value)
    .indices_per_buffer(m_painter_indices_per_buffer.m_value)
    .data_blocks_per_store_buffer(m_painter_data_blocks_per_buffer.m_value)
    .number_pools(m_painter_number_pools.m_value)
    .break_on_shader_change(m_painter_break_on_shader_change.m_value)
    .use_hw_clip_planes(m_use_hw_clip_planes.m_value)
    .vert_shader_use_switch(m_uber_vert_use_switch.m_value)
    .frag_shader_use_switch(m_uber_frag_use_switch.m_value)
    .blend_shader_use_switch(m_uber_blend_use_switch.m_value)
    .unpack_header_and_brush_in_frag_shader(m_unpack_header_and_brush_in_frag_shader.m_value)
    .data_store_backing(m_data_store_backing.m_value.m_value)
    .assign_layout_to_vertex_shader_inputs(m_assign_layout_to_vertex_shader_inputs.m_value)
    .assign_layout_to_varyings(m_assign_layout_to_varyings.m_value)
    .assign_binding_points(m_assign_binding_points.m_value)
    .use_ubo_for_uniforms(m_use_ubo_for_uniforms.m_value)
    .separate_program_for_discard(m_separate_program_for_discard.m_value)
    .non_dashed_stroke_shader_uses_discard(m_non_dashed_stroke_shader_uses_discard.m_value);

  m_backend = FASTUIDRAWnew fastuidraw::gl::PainterBackendGL(m_painter_params, m_painter_base_params);
  m_painter = FASTUIDRAWnew fastuidraw::Painter(m_backend);
  m_glyph_cache = FASTUIDRAWnew fastuidraw::GlyphCache(m_painter->glyph_atlas());
  m_glyph_selector = FASTUIDRAWnew fastuidraw::GlyphSelector(m_glyph_cache);
  m_ft_lib = FASTUIDRAWnew fastuidraw::FreetypeLib();

  if(m_print_painter_config.m_value)
    {
      std::cout << "\nPainterBackendGL configuration:\n";
#define LAZY(X) do { \
    std::cout << std::setw(40) << #X": " << std::setw(8) << m_backend->configuration_gl().X() \
              << "  (requested " << m_painter_params.X() << ")\n";           \
  } while(0)

      LAZY(attributes_per_buffer);
      LAZY(indices_per_buffer);
      LAZY(number_pools);
      LAZY(break_on_shader_change);
      LAZY(vert_shader_use_switch);
      LAZY(frag_shader_use_switch);
      LAZY(blend_shader_use_switch);
      LAZY(unpack_header_and_brush_in_frag_shader);
      LAZY(separate_program_for_discard);
      std::cout << "\n\nOptions affected by GL context\n";
      LAZY(use_hw_clip_planes);
      LAZY(data_blocks_per_store_buffer);
      LAZY(assign_layout_to_vertex_shader_inputs);
      LAZY(assign_layout_to_varyings);
      LAZY(use_ubo_for_uniforms);
      std::cout << std::setw(40) << "alignment:" << std::setw(8) << m_backend->configuration_base().alignment()
                << "  (requested " << m_painter_base_params.alignment()
                << ")\n" << std::setw(40) << "data_store_backing:"
                << std::setw(8) << string_from_data_store_type(m_backend->configuration_gl().data_store_backing())
                << "  (requested " << string_from_data_store_type(m_painter_params.data_store_backing())
                << ")\n\n\n";

      #undef LAZY
      const fastuidraw::PainterShaderSet &sh(m_painter->default_shaders());
      std::cout << "Default shader IDs:\n";

      std::cout << "\tGlyph Shaders:\n";
      print_glyph_shader_ids(sh.glyph_shader());

      std::cout << "\tAnisoptropic Glyph shaders\n";
      print_glyph_shader_ids(sh.glyph_shader_anisotropic());

      std::cout << "\tSolid StrokeShaders:\n";
      print_stroke_shader_ids(sh.stroke_shader());

      std::cout << "\tPixel Width Stroke Shaders:\n";
      print_stroke_shader_ids(sh.pixel_width_stroke_shader());

      std::cout << "\tDashed Stroke Shader:\n";
      print_dashed_stroke_shader_ids(sh.dashed_stroke_shader());

      std::cout << "\tPixel Width Dashed Stroke Shader:\n";
      print_dashed_stroke_shader_ids(sh.pixel_width_dashed_stroke_shader());

      std::cout << "\tFill Shader AA:" << sh.fill_shader().aa_shader()->tag() << "\n";
      std::cout << "\tFill Shader non-AA:" << sh.fill_shader().non_aa_shader()->tag() << "\n";
    }

  m_painter_params = m_backend->configuration_gl();
  m_painter->target_resolution(w, h);
  derived_init(w, h);

  #ifdef FASTUIDRAW_GL_USE_GLES
  {
    glClearDepthf(0.0f);
  }
  #else
  {
    glClearDepth(0.0);
  }
  #endif
}

void
sdl_painter_demo::
on_resize(int w, int h)
{
  glViewport(0, 0, w, h);
  m_painter->target_resolution(w, h);
}

void
sdl_painter_demo::
draw_text(const std::string &text, float pixel_size,
          fastuidraw::reference_counted_ptr<const fastuidraw::FontBase> font,
          fastuidraw::GlyphRender renderer,
          const fastuidraw::PainterData &draw)
{
  std::istringstream str(text);
  std::vector<fastuidraw::Glyph> glyphs;
  std::vector<fastuidraw::vec2> positions;
  std::vector<uint32_t> chars;
  fastuidraw::PainterAttributeData P;

  create_formatted_text(str, renderer, pixel_size, font,
                        m_glyph_selector, glyphs, positions, chars);
  P.set_data(fastuidraw::PainterAttributeDataFillerGlyphs(cast_c_array(positions),
                                                          cast_c_array(glyphs), pixel_size));
  m_painter->draw_glyphs(draw, P);
}
