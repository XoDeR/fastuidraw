/*!
 * \file filled_path.cpp
 * \brief file filled_path.cpp
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


#include <vector>
#include <list>
#include <map>
#include <algorithm>
#include <ctime>
#include <math.h>
#include <boost/dynamic_bitset.hpp>

#include <fastuidraw/tessellated_path.hpp>
#include <fastuidraw/path.hpp>
#include <fastuidraw/painter/filled_path.hpp>
#include <fastuidraw/painter/painter_attribute_data.hpp>
#include "../private/util_private.hpp"
#include "../private/util_private_ostream.hpp"
#include "../private/bounding_box.hpp"
#include "../private/clip.hpp"
#include "../../3rd_party/glu-tess/glu-tess.hpp"

/* Actual triangulation is handled by GLU-tess.
   The main complexity in creating a FilledPath
   comes from two elements:
    - handling overlapping edges
    - creating a hierarchy for creating triangulations
      and for culling.

   The first is needed because GLU-tess will fail
   if any two edges overlap (we say a pair of edges
   overlap if they intersect at more than just a single
   point). We handle this by observing that GLU-tess
   takes doubles but TessellatedPath is floats. When
   we feed the coordinates to GLU-tess, we offset the
   values by an amount that is visible in fp64 but not
   in fp32. In addition, we also want to merge points
   that are close in fp32 as well. The details are
   handled in CoordinateCoverter, PointHoard and
   tesser.

   The second is needed for primarily to speed up
   tessellation. If a TessellatedPath has a large
   number of vertices, then that is likely because
   it is a high level of detail and likely zoomed in
   a great deal. To handle that, we need only to
   have the triangulation of a smaller portion of
   it ready. Thus we break the original path into
   a hierarchy of paths. The partitioning is done
   a single half plane at a time. A contour from
   the original path is computed by simply removing
   any points on the wrong side of the half plane
   and inserting the points where the path crossed
   the half plane. The sub-path objects are computed
   via the class SubPath. The class SubsetPrivate
   is the one that represents an element in the
   hierarchy that is triangulated on demand.
 */

/* Values to define how to create Subset objects.
 */
namespace SubsetConstants
{
  enum
    {
      recursion_depth = 12,
      points_per_subset = 64
    };

  /* if negative, aspect ratio is not
     enfored.
   */
  const float size_max_ratio = 4.0f;
}

/* Values to decide how to create guiding boxes around
   contours within a Subset for the purpose of improving
   triangulation, see PointHoard methods.

   The total number of boxes, B, from N points
   satisfies:

   N / pts_per_box <= B <= N * L / pts_per_box

   where L = boxes_per_box / (boxes_per_box - 1)

   The cost of each guiding box is 4 edges.
   We need to make sure that we do not add too
   many boxes where too many of the added edges
   are from the guiding boxes.

   TODO: the main purpose is to decrease (or
   eliminate) long skinny triangles. Another
   way to decrease such triangle is to add a
   post-process step that identifies triangles
   fans coming from a single point, decide if
   the triangles are long and skinny and if so
   to run GLU-tess on that fan with a collection
   of guiding edges to improve the triangulation
   quality.
 */
namespace PointHoardConstants
{
  enum
    {
      points_per_guiding_box = 16,
      min_points_per_guiding_box = 4,
      guiding_boxes_per_guiding_box = 8
    };

  /* set to false to disable using guiding boxes.
     A guiding box adds a contour that does not
     affect the winding values for the purpose
     of localizing triangles made by GLU-tess
     even more. The localizing usually makes
     GLU-tess run SLOWER, but improves triangulation,
     i.e. reduces the number and scope of long
     skinny triangles.
   */
  const bool enable_guiding_boxes = false;

  /* if true, guiding boxes are made per PathContour::interpolator_base
     from the original Path. If false, guiding boxes are made from
     the SubPath::SubContour fed to PointHoard::generate_path()
   */
  const bool guiding_boxes_per_interpolator = true;
}

/* Constants for CoordinateConverter.
   CoordinateConverter's purpose is to remap
   the bounding box of a fastuidraw::TessellatedPath
   to [0, 2 ^ N] x [0,  2 ^ N]
   and then apply a fudge offset to the point
   that an fp64 sees but an fp32 does not.

   We do this to allow for the input TessellatedPath
   to have overlapping edges. The value for the
   fudge offset is to be incremented on each point.

   An fp32 has a 23-bit significand that allows it
   to represent any integer in the range [-2^24, 2^24]
   exactly. An fp64 has a 52 bit significand.

   We set N to be 24 and the fudginess to be 2^-20
   (leaving 9-bits for GLU to use for intersections).
 */
namespace CoordinateConverterConstants
{
  enum
    {
      log2_box_dim = 22,
      negative_log2_fudge = 20,
      box_dim = (1 << log2_box_dim),
    };
}

namespace
{
  bool
  fcn_non_zero_fill_rule(int w)
  {
    return w != 0;
  }

  bool
  fcn_complelemt_non_zero_fill_rule(int w)
  {
    return w == 0;
  }

  bool
  fcn_odd_even_fill_rule(int w)
  {
    return fastuidraw::t_abs(w) % 2 == 1;
  }

  bool
  fcn_complement_odd_even_fill_rule(int w)
  {
    return w % 2 == 0;
  }

  class per_winding_data:
    public fastuidraw::reference_counted<per_winding_data>::non_concurrent
  {
  public:
    explicit
    per_winding_data(int pwinding_number):
      m_count(0),
      m_winding_number(pwinding_number)
    {}

    void
    add_index(unsigned int idx)
    {
      m_indices.push_back(idx);
      ++m_count;
    }

    unsigned int
    count(void) const
    {
      return m_count;
    }

    int
    winding_number(void) const
    {
      return m_winding_number;
    }

    void
    fill_at(unsigned int &offset,
            fastuidraw::c_array<unsigned int> dest,
            fastuidraw::const_c_array<unsigned int> &sub_range)
    {
      assert(count() + offset <= dest.size());
      std::copy(m_indices.begin(), m_indices.end(), &dest[offset]);
      sub_range = dest.sub_array(offset, count());
      offset += count();
    }

  private:
    std::list<unsigned int> m_indices;
    unsigned int m_count;
    int m_winding_number;
  };

  typedef std::map<int, fastuidraw::reference_counted_ptr<per_winding_data> > winding_index_hoard;

  bool
  is_even(int v)
  {
    return (v % 2) == 0;
  }

  class CoordinateConverter
  {
  public:
    explicit
    CoordinateConverter(const fastuidraw::vec2 &fpmin, const fastuidraw::vec2 &fpmax)
    {
      fastuidraw::vecN<double, 2> delta, pmin, pmax;

      pmin = fastuidraw::vecN<double, 2>(fpmin);
      pmax = fastuidraw::vecN<double, 2>(fpmax);
      delta = pmax - pmin;
      m_scale = fastuidraw::vecN<double, 2>(1.0, 1.0) / delta;
      m_scale *= static_cast<double>(CoordinateConverterConstants::box_dim);
      m_translate = pmin;
      m_delta_fudge = ::exp2(static_cast<double>(-CoordinateConverterConstants::negative_log2_fudge));
      m_scale_f = fastuidraw::vec2(m_scale);
      m_translate_f = fastuidraw::vec2(m_translate);
    }

    fastuidraw::vecN<double, 2>
    apply(const fastuidraw::vec2 &pt, unsigned int fudge_count) const
    {
      fastuidraw::vecN<double, 2> r, qt(pt);
      double fudge;

      r = m_scale * (qt - m_translate);
      fudge = static_cast<double>(fudge_count) * m_delta_fudge;
      r.x() += fudge;
      r.y() += fudge;
      return r;
    }

    fastuidraw::ivec2
    iapply(const fastuidraw::vec2 &pt) const
    {
      fastuidraw::vec2 r;
      fastuidraw::ivec2 return_value;

      r = m_scale_f * (pt - m_translate_f);
      return_value.x() = static_cast<int>(r.x());
      return_value.y() = static_cast<int>(r.y());
      return return_value;
    }

    double
    fudge_delta(void) const
    {
      return m_delta_fudge;
    }

  private:
    double m_delta_fudge;
    fastuidraw::vecN<double, 2> m_scale, m_translate;
    fastuidraw::vec2 m_scale_f, m_translate_f;
  };

  enum
    {
      box_max_x_flag = 1,
      box_max_y_flag = 2,
      box_min_x_min_y = 0 | 0,
      box_min_x_max_y = 0 | box_max_y_flag,
      box_max_x_max_y = box_max_x_flag | box_max_y_flag,
      box_max_x_min_y = box_max_x_flag,
    };

  unsigned int
  box_next_neighbor(unsigned int v)
  {
    const unsigned int values[4]=
    {
      /* 0 is box_min_x_min_y */ box_min_x_max_y,
      /* 1 is box_max_x_min_y */ box_min_x_min_y,
      /* 2 is box_min_x_max_y */ box_max_x_max_y,
      /* 3 is box_max_x_max_y */ box_max_x_min_y,
    };
    assert(v <= 3);
    return values[v];
  }

  class SubPath
  {
  public:
    class SubContourPoint
    {
    public:
      enum on_boundary_t
        {
          on_min_boundary,
          on_max_boundary,
          not_on_boundary
        };

      explicit
      SubContourPoint(const fastuidraw::vec2 &p = fastuidraw::vec2(),
                      bool start = false):
        m_pt(p),
        m_start_tessellated_edge(start),
        m_boundary_type(not_on_boundary, not_on_boundary),
        m_corner_point_type(4)
      {}

      SubContourPoint(const SubContourPoint &a,
                      const SubContourPoint &b,
                      const fastuidraw::vec2 &pt,
                      int split_coordinate,
                      enum on_boundary_t tp);

      const fastuidraw::vec2&
      pt(void) const
      {
        return m_pt;
      }

      bool
      start_tessellated_edge(void) const
      {
        return m_start_tessellated_edge;
      }

      bool
      is_corner_point(void) const
      {
        return m_boundary_type[0] != not_on_boundary
          && m_boundary_type[1] != not_on_boundary;
      }

      uint32_t
      corner_point_type(void) const
      {
        assert(is_corner_point());
        assert(m_corner_point_type <= 3);
        return m_corner_point_type;
      }

    private:
      fastuidraw::vec2 m_pt;
      bool m_start_tessellated_edge;
      fastuidraw::vecN<enum on_boundary_t, 2> m_boundary_type;
      int m_corner_point_type;
    };

    typedef std::vector<SubContourPoint> SubContour;

    explicit
    SubPath(const fastuidraw::TessellatedPath &P);

    const std::vector<SubContour>&
    contours(void) const
    {
      return m_contours;
    }

    const fastuidraw::BoundingBox&
    bounds(void) const
    {
      return m_bounds;
    }

    unsigned int
    total_points(void) const
    {
      return m_total_points;
    }

    int
    winding_start(void) const
    {
      return m_winding_start;
    }

    fastuidraw::vecN<SubPath*, 2>
    split(void) const;

  private:
    SubPath(const fastuidraw::BoundingBox &bb,
            std::vector<SubContour> &contours,
            int winding_start);

    int
    choose_splitting_coordinate(fastuidraw::vec2 mid_pt) const;

    static
    void
    copy_contour(SubContour &dst,
                 const fastuidraw::TessellatedPath &src, unsigned int C);

    static
    void
    split_contour(const SubContour &src,
                  int splitting_coordinate, float spitting_value,
                  SubContour &minC, SubContour &maxC,
                  int &C0_winding_start, int &C1_winding_start);

    static
    int
    post_process_sub_contour(SubContour &C);

    static
    fastuidraw::vec2
    compute_spit_point(fastuidraw::vec2 a,
                       fastuidraw::vec2 b,
                       int splitting_coordinate, float spitting_value);

    unsigned int m_total_points;
    fastuidraw::BoundingBox m_bounds;
    std::vector<SubContour> m_contours;
    int m_winding_start;
  };

  class WindingSet
  {
  public:
    WindingSet(void):
      m_begin(0),
      m_end(0)
    {}

    void
    clear(void)
    {
      m_begin = m_end = 0;
      m_bits.clear();
    }

    // set to encode the -complement- of the passed fill rule.
    void
    extract_from_fill_fule(int min_value, int max_value,
                           const fastuidraw::CustomFillRuleBase &fill_rule,
                           bool flip);

    void
    extract_from_set(const std::set<int> &in_values);

    bool
    have_common_bit(const WindingSet &obj) const;

    int
    begin(void) const
    {
      return m_begin;
    }

    int
    end(void) const
    {
      return m_end;
    }

    bool
    has(int w) const;

  private:
    boost::dynamic_bitset<> m_bits;
    int m_begin, m_end;
  };

  class FillPoint
  {
  public:
    fastuidraw::vec2 m_pt;
    std::set<int> m_winding;
  };

  class PointHoard:fastuidraw::noncopyable
  {
  public:
    typedef std::vector<unsigned int> Contour;
    typedef std::list<Contour> Path;
    typedef std::vector<fastuidraw::uvec4> BoundingBoxes;

    explicit
    PointHoard(const fastuidraw::BoundingBox &bounds,
               std::vector<FillPoint> &pts):
      m_converter(bounds.min_point(), bounds.max_point()),
      m_pts(pts)
    {
      assert(!bounds.empty());
    }

    unsigned int
    fetch(const fastuidraw::vec2 &pt);

    void
    generate_path(const SubPath &input, Path &output,
                  BoundingBoxes &bounding_boxes);

    const fastuidraw::vec2&
    operator[](unsigned int v) const
    {
      assert(v < m_pts.size());
      return m_pts[v].m_pt;
    }

    const CoordinateConverter&
    converter(void) const
    {
      return m_converter;
    }

    void
    add_to_winding_set(unsigned int v, int winding)
    {
      assert(v < m_pts.size());
      m_pts[v].m_winding.insert(winding);
    }

  private:
    void
    pre_process_boxes(std::vector<fastuidraw::BoundingBox> &boxes,
                      unsigned int cnt);

    void
    process_bounding_boxes(const std::vector<fastuidraw::BoundingBox> &boxes,
                           BoundingBoxes &bounding_boxes);

    void
    generate_contour(const SubPath::SubContour &input,
                     Contour &output,
                     BoundingBoxes &bounding_boxes);

    CoordinateConverter m_converter;
    std::map<fastuidraw::ivec2, unsigned int> m_map;
    std::vector<FillPoint> &m_pts;
  };

  class tesser:fastuidraw::noncopyable
  {
  protected:
    explicit
    tesser(PointHoard &points);

    virtual
    ~tesser(void);

    void
    start(void);

    void
    stop(void);

    void
    add_path(const PointHoard::Path &P);

    void
    add_path_boundary(const SubPath &P);

    void
    add_bounding_box_path(const PointHoard::BoundingBoxes &P);

    bool
    triangulation_failed(void)
    {
      return m_triangulation_failed;
    }

    virtual
    void
    on_begin_polygon(int winding_number) = 0;

    virtual
    void
    add_vertex_to_polygon(unsigned int vertex) = 0;

    virtual
    FASTUIDRAW_GLUboolean
    fill_region(int winding_number) = 0;

  protected:
    void
    add_to_winding_set(unsigned int v, int winding)
    {
      m_points.add_to_winding_set(v, winding);
    }

  private:
    void
    add_contour(const PointHoard::Contour &C);

    void
    add_bounding_box_path_element(const fastuidraw::uvec4 &box);

    void
    add_triangle(unsigned int a, unsigned int b, unsigned int c)
    {
      add_vertex_to_polygon(a);
      add_vertex_to_polygon(b);
      add_vertex_to_polygon(c);
    }

    static
    void
    begin_callBack(FASTUIDRAW_GLUenum type, int winding_number, void *tess);

    static
    void
    vertex_callBack(unsigned int vertex_data, void *tess);

    static
    void
    combine_callback(double x, double y, unsigned int data[4],
                     double weight[4],  unsigned int *outData,
                     void *tess);

    static
    FASTUIDRAW_GLUboolean
    winding_callBack(int winding_number, void *tess);

    unsigned int
    add_point_to_store(const fastuidraw::vec2 &p);

    bool
    temp_verts_non_degenerate_triangle(void);

    unsigned int m_point_count;
    fastuidraw_GLUtesselator *m_tess;
    PointHoard &m_points;
    fastuidraw::vecN<unsigned int, 3> m_temp_verts;
    unsigned int m_temp_vert_count;
    bool m_triangulation_failed;
  };

  class non_zero_tesser:private tesser
  {
  public:
    static
    bool
    execute_path(PointHoard &points,
                 const PointHoard::Path &P,
                 const PointHoard::BoundingBoxes &boxes,
                 const SubPath &path,
                 winding_index_hoard &hoard)
    {
      non_zero_tesser NZ(points, P, boxes, path, hoard);
      return NZ.triangulation_failed();
    }

  private:
    non_zero_tesser(PointHoard &points,
                    const PointHoard::Path &P,
                    const PointHoard::BoundingBoxes &boxes,
                    const SubPath &path,
                    winding_index_hoard &hoard);

    virtual
    void
    on_begin_polygon(int winding_number);

    virtual
    void
    add_vertex_to_polygon(unsigned int vertex);

    virtual
    FASTUIDRAW_GLUboolean
    fill_region(int winding_number);

    int m_winding_start;
    winding_index_hoard &m_hoard;
    int m_current_winding;
    fastuidraw::reference_counted_ptr<per_winding_data> m_current_indices;
  };

  class zero_tesser:private tesser
  {
  public:
    static
    bool
    execute_path(PointHoard &points,
                 const PointHoard::Path &P,
                 const PointHoard::BoundingBoxes &boxes,
                 const SubPath &path,
                 winding_index_hoard &hoard)
    {
      zero_tesser Z(points, P, boxes, path, hoard);
      return Z.triangulation_failed();
    }

  private:

    zero_tesser(PointHoard &points,
                const PointHoard::Path &P,
                const PointHoard::BoundingBoxes &boxes,
                const SubPath &path,
                winding_index_hoard &hoard);

    virtual
    void
    on_begin_polygon(int winding_number);

    virtual
    void
    add_vertex_to_polygon(unsigned int vertex);

    virtual
    FASTUIDRAW_GLUboolean
    fill_region(int winding_number);

    fastuidraw::reference_counted_ptr<per_winding_data> &m_indices;
  };

  class builder:fastuidraw::noncopyable
  {
  public:
    explicit
    builder(const SubPath &P, std::vector<FillPoint> &pts);

    ~builder();

    void
    fill_indices(std::vector<unsigned int> &indices,
                 std::map<int, fastuidraw::const_c_array<unsigned int> > &winding_map,
                 unsigned int &even_non_zero_start,
                 unsigned int &zero_start);

    bool
    triangulation_failed(void)
    {
      return m_failed;
    }

  private:
    winding_index_hoard m_hoard;
    PointHoard m_points;
    bool m_failed;
  };

  class AttributeDataMerger:public fastuidraw::PainterAttributeDataFiller
  {
  public:
    AttributeDataMerger(const fastuidraw::PainterAttributeData &a,
                        const fastuidraw::PainterAttributeData &b):
      m_a(a), m_b(b)
    {
    }

    virtual
    void
    compute_sizes(unsigned int &number_attributes,
                  unsigned int &number_indices,
                  unsigned int &number_attribute_chunks,
                  unsigned int &number_index_chunks,
                  unsigned int &number_z_increments) const;

    virtual
    void
    fill_data(fastuidraw::c_array<fastuidraw::PainterAttribute> attributes,
              fastuidraw::c_array<fastuidraw::PainterIndex> indices,
              fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterAttribute> > attrib_chunks,
              fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterIndex> > index_chunks,
              fastuidraw::c_array<unsigned int> zincrements,
              fastuidraw::c_array<int> index_adjusts) const;

    static
    void
    fill_winding_data(const std::vector<WindingSet> &a,
                      const std::vector<WindingSet> &b,
                      std::vector<WindingSet> &dst);

    const fastuidraw::PainterAttributeData &m_a, &m_b;
  };

  class AttributeDataFiller:public fastuidraw::PainterAttributeDataFiller
  {
  public:
    std::vector<FillPoint> m_points;

    /* Carefully organize indices as follows:
       - first all elements with odd winding number
       - then all elements with even and non-zero winding number
       - then all element with zero winding number.
       By doing so, the following are continuous in the array:
       - non-zero
       - odd-even fill rule
       - complement of odd-even fill
       - complement of non-zero
     */
    std::vector<unsigned int> m_indices;
    fastuidraw::const_c_array<unsigned int> m_nonzero_winding_indices;
    fastuidraw::const_c_array<unsigned int> m_zero_winding_indices;
    fastuidraw::const_c_array<unsigned int> m_odd_winding_indices;
    fastuidraw::const_c_array<unsigned int> m_even_winding_indices;

    /* m_per_fill[w] gives the indices to the triangles
       with the winding number w. The value points into
       indices
    */
    std::map<int, fastuidraw::const_c_array<unsigned int> > m_per_fill;

    virtual
    void
    compute_sizes(unsigned int &number_attributes,
                  unsigned int &number_indices,
                  unsigned int &number_attribute_chunks,
                  unsigned int &number_index_chunks,
                  unsigned int &number_z_increments) const;
    virtual
    void
    fill_data(fastuidraw::c_array<fastuidraw::PainterAttribute> attributes,
              fastuidraw::c_array<fastuidraw::PainterIndex> indices,
              fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterAttribute> > attrib_chunks,
              fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterIndex> > index_chunks,
              fastuidraw::c_array<unsigned int> zincrements,
              fastuidraw::c_array<int> index_adjusts) const;

    void
    fill_winding_data(std::vector<WindingSet> &dst) const;

    static
    fastuidraw::PainterAttribute
    generate_attribute(const FillPoint &src)
    {
      fastuidraw::PainterAttribute dst;

      dst.m_attrib0 = fastuidraw::pack_vec4(src.m_pt.x(), src.m_pt.y(), 1.0f, 0.0f);
      dst.m_attrib1 = fastuidraw::uvec4(0u, 0u, 0u, 0u);
      dst.m_attrib2 = fastuidraw::uvec4(0u, 0u, 0u, 0u);

      return dst;
    }
  };

  class ScratchSpacePrivate
  {
  public:
    std::vector<fastuidraw::vec3> m_adjusted_clip_eqs;
    std::vector<fastuidraw::vec2> m_clipped_rect;

    fastuidraw::vecN<std::vector<fastuidraw::vec2>, 2> m_clip_scratch_vec2s;
    std::vector<float> m_clip_scratch_floats;
  };

  class SubsetPrivate
  {
  public:
    ~SubsetPrivate(void);

    SubsetPrivate(SubPath *P, int max_recursion,
                  std::vector<SubsetPrivate*> &out_values);

    unsigned int
    select_subsets(ScratchSpacePrivate &scratch,
                   fastuidraw::const_c_array<fastuidraw::vec3> clip_equations,
                   const fastuidraw::float3x3 &clip_matrix_local,
                   unsigned int max_attribute_cnt,
                   unsigned int max_index_cnt,
                   fastuidraw::c_array<unsigned int> dst);

    void
    make_ready(void);

    fastuidraw::const_c_array<int>
    winding_numbers(void) const
    {
      assert(m_painter_data != NULL);
      return fastuidraw::make_c_array(m_winding_numbers);
    }

    const fastuidraw::PainterAttributeData&
    painter_data(void) const
    {
      assert(m_painter_data != NULL);
      return *m_painter_data;
    }

    const std::vector<WindingSet>&
    windings_per_pt(void) const
    {
      return m_windings_per_pt;
    }

  private:
    void
    select_subsets_implement(ScratchSpacePrivate &scratch,
                             fastuidraw::c_array<unsigned int> dst,
                             unsigned int max_attribute_cnt,
                             unsigned int max_index_cnt,
                             unsigned int &current);

    void
    select_subsets_all_unculled(fastuidraw::c_array<unsigned int> dst,
                                unsigned int max_attribute_cnt,
                                unsigned int max_index_cnt,
                                unsigned int &current);

    void
    make_ready_from_children(void);

    void
    make_ready_from_sub_path(void);

    /* m_ID represents an index into the std::vector<>
       passed into create_hierarchy() where this element
       is found.
     */
    unsigned int m_ID;

    /* The bounds of this SubsetPrivate used in
       select_subsets().
     */
    fastuidraw::BoundingBox m_bounds;

    /* if this SubsetPrivate has children then
       m_painter_data is made by "merging" the
       data of m_painter_data from m_children[0]
       and m_children[1]. We do this merging so
       that we can avoid recursing if the entirity
       of the bounding box is contained in the
       clipping region.
     */
    fastuidraw::PainterAttributeData *m_painter_data;
    std::vector<int> m_winding_numbers;

    /* for each point (indexed as in m_painter_data),
       we store what winding values each vertex has
     */
    std::vector<WindingSet> m_windings_per_pt;

    bool m_sizes_ready;
    unsigned int m_num_attributes;
    unsigned int m_largest_index_block;

    /* m_sub_path is non-NULL only if this SubsetPrivate
       has no children. In addition, it is set to NULL
       and deleted when m_painter_data is created from
       it.
     */
    SubPath *m_sub_path;
    fastuidraw::vecN<SubsetPrivate*, 2> m_children;
  };

  class DataWriterPrivate
  {
  public:
    class per_index_chunk
    {
    public:
      explicit
      per_index_chunk(fastuidraw::const_c_array<fastuidraw::PainterIndex> indices,
                      unsigned int attrib_chunk):
        m_indices(indices),
        m_attrib_chunk(attrib_chunk)
      {}

      fastuidraw::const_c_array<fastuidraw::PainterIndex> m_indices;
      unsigned int m_attrib_chunk;
    };

    class per_attrib_chunk
    {
    public:
      explicit
      per_attrib_chunk(const SubsetPrivate *d):
        m_attribs(d->painter_data().attribute_data_chunk(0)),
        m_per_pt_winding_set(fastuidraw::make_c_array(d->windings_per_pt()))
      {}

      fastuidraw::const_c_array<fastuidraw::PainterAttribute> m_attribs;
      fastuidraw::const_c_array<WindingSet> m_per_pt_winding_set;
    };

    std::vector<unsigned int> m_subset_selector;
    std::vector<per_attrib_chunk> m_attribute_chunks;
    std::vector<per_index_chunk> m_index_chunks;
    WindingSet m_complement_winding_rule, m_winding_rule;
  };

  class FilledPathPrivate
  {
  public:
    explicit
    FilledPathPrivate(const fastuidraw::TessellatedPath &P);

    ~FilledPathPrivate();

    SubsetPrivate *m_root;
    std::vector<SubsetPrivate*> m_subsets;
  };
}

////////////////////////////////////////
// SubPath::SubContourPoint methods
SubPath::SubContourPoint::
SubContourPoint(const SubContourPoint &a,
                const SubContourPoint &b,
                const fastuidraw::vec2 &pt,
                int split_coordinate,
                enum on_boundary_t tp):
  m_pt(pt),
  m_start_tessellated_edge(true)
{
  int unsplit_coordinate(1 - split_coordinate);
  if(a.m_boundary_type[unsplit_coordinate] == b.m_boundary_type[unsplit_coordinate])
    {
      m_boundary_type[unsplit_coordinate] = a.m_boundary_type[unsplit_coordinate];
    }
  else
    {
      m_boundary_type[unsplit_coordinate] = not_on_boundary;
    }
  m_boundary_type[split_coordinate] = tp;

  if(is_corner_point())
    {
      m_corner_point_type = 0;
      if(m_boundary_type.x() == on_max_boundary)
        {
          m_corner_point_type |= box_max_x_flag;
        }

      if(m_boundary_type.y() == on_max_boundary)
        {
          m_corner_point_type |= box_max_y_flag;
        }
    }
  else
    {
      m_corner_point_type = 4;
    }
}



/////////////////////////////////////
// SubPath methods
SubPath::
SubPath(const fastuidraw::BoundingBox &bb,
        std::vector<SubContour> &contours,
        int winding_start):
  m_total_points(0),
  m_bounds(bb),
  m_winding_start(winding_start)
{
  m_contours.swap(contours);
  for(std::vector<SubContour>::const_iterator c_iter = m_contours.begin(),
        c_end = m_contours.end(); c_iter != c_end; ++c_iter)
    {
      m_total_points += c_iter->size();
    }
}

SubPath::
SubPath(const fastuidraw::TessellatedPath &P):
  m_total_points(0),
  m_bounds(P.bounding_box_min(),
           P.bounding_box_max()),
  m_contours(P.number_contours()),
  m_winding_start(0)
{
  for(unsigned int c = 0, endc = m_contours.size(); c < endc; ++c)
    {
      copy_contour(m_contours[c], P, c);
      m_total_points += m_contours[c].size();
    }
}

void
SubPath::
copy_contour(SubContour &dst,
             const fastuidraw::TessellatedPath &src, unsigned int C)
{
  for(unsigned int e = 0, ende = src.number_edges(C); e < ende; ++e)
    {
      fastuidraw::range_type<unsigned int> R;

      R = src.edge_range(C, e);
      dst.push_back(SubContourPoint(src.point_data()[R.m_begin].m_p, true));
      for(unsigned int v = R.m_begin + 1; v + 1 < R.m_end; ++v)
        {
          SubContourPoint pt(src.point_data()[v].m_p);
          dst.push_back(pt);
        }
    }
}

int
SubPath::
choose_splitting_coordinate(fastuidraw::vec2 mid_pt) const
{
  /* do not allow the box to be too far from being a square.
     TODO: if the balance of points heavily favors the other
     side, we should ignore the size_max_ratio. Perhaps a
     wieght factor between the different in # of points
     of the sides and the ratio?
   */
  if(SubsetConstants::size_max_ratio > 0.0f)
    {
      fastuidraw::vec2 wh;
      wh = m_bounds.max_point() - m_bounds.min_point();
      if(wh.x() >= SubsetConstants::size_max_ratio * wh.y())
        {
          return 0;
        }
      else if(wh.y() >= SubsetConstants::size_max_ratio * wh.x())
        {
          return 1;
        }
    }

  /* first find which of splitting in X or splitting in Y
     is optimal.
   */
  fastuidraw::ivec2 number_points_before(0, 0);
  fastuidraw::ivec2 number_points_after(0, 0);
  fastuidraw::ivec2 number_points;

  for(std::vector<SubContour>::const_iterator c_iter = m_contours.begin(),
        c_end = m_contours.end(); c_iter != c_end; ++c_iter)
    {
      fastuidraw::vec2 prev_pt(c_iter->back().pt());
      for(SubContour::const_iterator iter = c_iter->begin(),
            end = c_iter->end(); iter != end; ++iter)
        {
          fastuidraw::vec2 pt(iter->pt());
          for(int i = 0; i < 2; ++i)
            {
              bool prev_b, b;

              prev_b = prev_pt[i] < mid_pt[i];
              b = pt[i] < mid_pt[i];

              if(b || pt[i] == mid_pt[i])
                {
                  ++number_points_before[i];
                }

              if(!b || pt[i] == mid_pt[i])
                {
                  ++number_points_after[i];
                }

              if(prev_pt[i] != mid_pt[i] && prev_b != b)
                {
                  ++number_points_before[i];
                  ++number_points_after[i];
                }
            }
          prev_pt = pt;
        }
    }

  /* choose a splitting that:
      - minimizes number_points_before[i] + number_points_after[i]
   */
  number_points = number_points_before + number_points_after;
  if(number_points.x() < number_points.y())
    {
      return 0;
    }
  else
    {
      return 1;
    }
}

fastuidraw::vec2
SubPath::
compute_spit_point(fastuidraw::vec2 a, fastuidraw::vec2 b,
                   int splitting_coordinate, float splitting_value)
{
  float t, n, d, aa, bb;
  fastuidraw::vec2 return_value;

  n = splitting_value - a[splitting_coordinate];
  d = b[splitting_coordinate] - a[splitting_coordinate];
  t = n / d;

  return_value[splitting_coordinate] = splitting_value;

  aa = a[1 - splitting_coordinate];
  bb = b[1 - splitting_coordinate];
  return_value[1 - splitting_coordinate] = (1.0f - t) * aa + t * bb;

  return return_value;
}

void
SubPath::
split_contour(const SubContour &src,
              int splitting_coordinate, float splitting_value,
              SubContour &C0, SubContour &C1,
              int &C0_winding_start, int &C1_winding_start)
{
  SubContourPoint prev_pt(src.back());
  for(SubContour::const_iterator iter = src.begin(),
        end = src.end(); iter != end; ++iter)
    {
      bool b0, prev_b0;
      bool b1, prev_b1;
      fastuidraw::vec2 split_pt;
      const SubContourPoint &pt(*iter);

      prev_b0 = prev_pt.pt()[splitting_coordinate] <= splitting_value;
      b0 = pt.pt()[splitting_coordinate] <= splitting_value;

      prev_b1 = prev_pt.pt()[splitting_coordinate] >= splitting_value;
      b1 = pt.pt()[splitting_coordinate] >= splitting_value;

      if(prev_b0 != b0 || prev_b1 != b1)
        {
          split_pt = compute_spit_point(prev_pt.pt(), pt.pt(),
                                        splitting_coordinate, splitting_value);
        }

      if(prev_b0 != b0)
        {
          SubContourPoint s(prev_pt, pt, split_pt, splitting_coordinate, SubContourPoint::on_max_boundary);
          C0.push_back(s);
        }

      if(b0)
        {
          C0.push_back(pt);
        }

      if(prev_b1 != b1)
        {
          SubContourPoint s(prev_pt, pt, split_pt, splitting_coordinate, SubContourPoint::on_min_boundary);
          C1.push_back(s);
        }

      if(b1)
        {
          C1.push_back(pt);
        }

      prev_pt = pt;
    }

  C0_winding_start += post_process_sub_contour(C0);
  C1_winding_start += post_process_sub_contour(C1);
}


int
SubPath::
post_process_sub_contour(SubContour &C)
{
  /* if all edges of C are along the boundary,
     collapse C to nothing and return the number
     of times C wraps around the box.
   */
  unsigned int prev_corner_type;
  int forwards_counter(0), backwards_counter(0);

  if(C.empty() || !C.back().is_corner_point())
    {
      return 0;
    }

  /* IDEA: going to a next neighbor from prev_corner_type
     increments counter, going to a previous neighbor
     decrements the counter. The counter % 4 gives us
     the number of times the contour went around the
     box.
   */
  prev_corner_type = C.back().corner_point_type();
  for(SubContour::const_iterator iter = C.begin(),
        end = C.end(); iter != end; ++iter)
    {
      unsigned int corner_type;

      if(!iter->is_corner_point())
        {
          return 0;
        }

      corner_type = iter->corner_point_type();
      if(corner_type == box_next_neighbor(prev_corner_type))
        {
          ++forwards_counter;
        }
      else if(prev_corner_type == box_next_neighbor(corner_type))
        {
          ++backwards_counter;
        }
      else
        {
          return 0;
        }
      prev_corner_type = corner_type;
    }

  int counter;
  counter = backwards_counter - forwards_counter;
  if(counter % 4 == 0)
    {
      C.clear();
      return counter / 4;
    }

  return 0;
}


fastuidraw::vecN<SubPath*, 2>
SubPath::
split(void) const
{
  fastuidraw::vecN<SubPath*, 2> return_value(NULL, NULL);
  fastuidraw::vec2 mid_pt;
  int splitting_coordinate;

  mid_pt = 0.5f * (m_bounds.max_point() + m_bounds.min_point());
  splitting_coordinate = choose_splitting_coordinate(mid_pt);

  /* now split each contour.
   */
  fastuidraw::vec2 B0_max, B1_min;
  B0_max[1 - splitting_coordinate] = m_bounds.max_point()[1 - splitting_coordinate];
  B0_max[splitting_coordinate] = mid_pt[splitting_coordinate];

  B1_min[1 - splitting_coordinate] = m_bounds.min_point()[1 - splitting_coordinate];
  B1_min[splitting_coordinate] = mid_pt[splitting_coordinate];

  fastuidraw::BoundingBox B0(m_bounds.min_point(), B0_max);
  fastuidraw::BoundingBox B1(B1_min, m_bounds.max_point());
  std::vector<SubContour> C0, C1;
  int C0_winding_start(0), C1_winding_start(0);

  C0.reserve(m_contours.size());
  C1.reserve(m_contours.size());
  for(std::vector<SubContour>::const_iterator c_iter = m_contours.begin(),
        c_end = m_contours.end(); c_iter != c_end; ++c_iter)
    {
      C0.push_back(SubContour());
      C1.push_back(SubContour());
      split_contour(*c_iter, splitting_coordinate,
                    mid_pt[splitting_coordinate],
                    C0.back(), C1.back(),
                    C0_winding_start, C1_winding_start);

      if(C0.back().empty())
        {
          C0.pop_back();
        }

      if(C1.back().empty())
        {
          C1.pop_back();
        }
    }

  return_value[0] = FASTUIDRAWnew SubPath(B0, C0, C0_winding_start + m_winding_start);
  return_value[1] = FASTUIDRAWnew SubPath(B1, C1, C1_winding_start + m_winding_start);

  return return_value;
}

//////////////////////////////////////
// PointHoard methods
unsigned int
PointHoard::
fetch(const fastuidraw::vec2 &pt)
{
  std::map<fastuidraw::ivec2, unsigned int>::iterator iter;
  fastuidraw::ivec2 ipt;
  unsigned int return_value;

  ipt = m_converter.iapply(pt);
  iter = m_map.find(ipt);
  if(iter != m_map.end())
    {
      return_value = iter->second;
    }
  else
    {
      FillPoint p;
      return_value = m_pts.size();
      p.m_pt = pt;
      m_pts.push_back(p);
      m_map[ipt] = return_value;
    }
  return return_value;
}

void
PointHoard::
generate_path(const SubPath &input, Path &output,
              BoundingBoxes &bounding_box_path)
{
  output.clear();
  const std::vector<SubPath::SubContour> &contours(input.contours());
  for(std::vector<SubPath::SubContour>::const_iterator iter = contours.begin(),
        end = contours.end(); iter != end; ++iter)
    {
      const SubPath::SubContour &C(*iter);
      output.push_back(Contour());
      generate_contour(C, output.back(), bounding_box_path);
    }
}

void
PointHoard::
generate_contour(const SubPath::SubContour &C, Contour &output,
                 BoundingBoxes &bounding_box_path)
{
  std::vector<fastuidraw::BoundingBox> boxes(1);
  unsigned int total_cnt(0), cnt(0);

  for(unsigned int v = 0, endv = C.size(); v < endv; ++v,  ++cnt, ++total_cnt)
    {
      /* starting a tessellated edge means that we
         restart our current building boxes.
       */
      if(PointHoardConstants::guiding_boxes_per_interpolator
         && PointHoardConstants::enable_guiding_boxes
         && v != 0 && C[v].start_tessellated_edge())
        {
          pre_process_boxes(boxes, cnt);
          if(total_cnt >= PointHoardConstants::min_points_per_guiding_box)
            {
              process_bounding_boxes(boxes, bounding_box_path);
            }
          boxes.clear();
          boxes.resize(1);
          cnt = 0;
          total_cnt = 0;
        }

      output.push_back(fetch(C[v].pt()));
      boxes.back().union_point(C[v].pt());
      if(cnt == PointHoardConstants::points_per_guiding_box)
        {
          cnt = 0;
          boxes.push_back(fastuidraw::BoundingBox());
        }
    }

  if(PointHoardConstants::enable_guiding_boxes)
    {
      pre_process_boxes(boxes, cnt);
      if(total_cnt >= PointHoardConstants::min_points_per_guiding_box)
        {
          process_bounding_boxes(boxes, bounding_box_path);
        }
    }
}

void
PointHoard::
pre_process_boxes(std::vector<fastuidraw::BoundingBox> &boxes, unsigned int cnt)
{
  if(cnt <= 4 && boxes.size() > 1)
    {
      fastuidraw::BoundingBox B;
      B = boxes.back();
      boxes.pop_back();
      boxes.back().union_box(B);
    }
  else if(boxes.size() == 1 && cnt <= 2)
    {
      boxes.pop_back();
    }
}

void
PointHoard::
process_bounding_boxes(const std::vector<fastuidraw::BoundingBox> &in_boxes,
                       BoundingBoxes &bounding_box_path)
{
  std::vector<fastuidraw::BoundingBox> boxes_of_boxes(1);
  unsigned int total_cnt(0), cnt(0);

  for(unsigned int i = 0, endi = in_boxes.size(); i < endi; ++i, ++cnt, ++total_cnt)
    {
      fastuidraw::vec2 sz;
      assert(!in_boxes[i].empty());

      /* get/save the positions of the box*/
      bounding_box_path.push_back(fastuidraw::uvec4());
      for(unsigned int k = 0; k < 4; ++k)
        {
          fastuidraw::vec2 pt;

          if(k & box_max_x_flag)
            {
              pt.x() = in_boxes[i].max_point().x();
            }
          else
            {
              pt.x() = in_boxes[i].min_point().x();
            }

          if(k & box_max_y_flag)
            {
              pt.y() = in_boxes[i].max_point().y();
            }
          else
            {
              pt.y() = in_boxes[i].min_point().y();
            }
          bounding_box_path.back()[k] = fetch(pt);
        }

      boxes_of_boxes.back().union_box(in_boxes[i]);
      if(cnt == PointHoardConstants::guiding_boxes_per_guiding_box)
        {
          cnt = 0;
          boxes_of_boxes.push_back(fastuidraw::BoundingBox());
        }
    }

  pre_process_boxes(boxes_of_boxes, cnt);
  if(total_cnt >= PointHoardConstants::guiding_boxes_per_guiding_box)
    {
      process_bounding_boxes(boxes_of_boxes, bounding_box_path);
    }
}

////////////////////////////////////////
// tesser methods
tesser::
tesser(PointHoard &points):
  m_point_count(0),
  m_points(points),
  m_triangulation_failed(false)
{
  m_tess = fastuidraw_gluNewTess;
  fastuidraw_gluTessCallbackBegin(m_tess, &begin_callBack);
  fastuidraw_gluTessCallbackVertex(m_tess, &vertex_callBack);
  fastuidraw_gluTessCallbackCombine(m_tess, &combine_callback);
  fastuidraw_gluTessCallbackFillRule(m_tess, &winding_callBack);
  fastuidraw_gluTessPropertyBoundaryOnly(m_tess, FASTUIDRAW_GLU_FALSE);
}

tesser::
~tesser(void)
{
  fastuidraw_gluDeleteTess(m_tess);
}


void
tesser::
start(void)
{
  fastuidraw_gluTessBeginPolygon(m_tess, this);
}

void
tesser::
stop(void)
{
  fastuidraw_gluTessEndPolygon(m_tess);
}

void
tesser::
add_path(const PointHoard::Path &P)
{
  for(PointHoard::Path::const_iterator iter = P.begin(),
        end = P.end(); iter != end; ++iter)
    {
      add_contour(*iter);
    }
}

void
tesser::
add_contour(const PointHoard::Contour &C)
{
  fastuidraw_gluTessBeginContour(m_tess, FASTUIDRAW_GLU_TRUE);
  for(unsigned int v = 0, endv = C.size(); v < endv; ++v)
    {
      fastuidraw::vecN<double, 2> p;
      unsigned int I;

      /* TODO: Incrementing the amount by which to apply
         fudge is not the correct thing to do. Rather, we
         should only increment and apply fudge on overlapping
         and degenerate edges.
      */
      I = C[v];
      p = m_points.converter().apply(m_points[I], m_point_count);
      ++m_point_count;

      fastuidraw_gluTessVertex(m_tess, p.x(), p.y(), I);
    }
  fastuidraw_gluTessEndContour(m_tess);
}

void
tesser::
add_path_boundary(const SubPath &P)
{
  fastuidraw::vec2 pmin, pmax;
  unsigned int src[4] =
    {
      box_min_x_min_y,
      box_min_x_max_y,
      box_max_x_max_y,
      box_max_x_min_y,
    };

  pmin = P.bounds().min_point();
  pmax = P.bounds().max_point();

  fastuidraw_gluTessBeginContour(m_tess, FASTUIDRAW_GLU_TRUE);
  for(unsigned int i = 0; i < 4; ++i)
    {
      double slack, x, y;
      unsigned int k;
      fastuidraw::vec2 p;

      slack = static_cast<double>(m_point_count) * m_points.converter().fudge_delta();
      k = src[i];
      if(k & box_max_x_flag)
        {
          x = slack + static_cast<double>(CoordinateConverterConstants::box_dim);
          p.x() = pmax.x();
        }
      else
        {
          x = -slack;
          p.x() = pmin.x();
        }

      if(k & box_max_y_flag)
        {
          y = slack + static_cast<double>(CoordinateConverterConstants::box_dim);
          p.y() = pmax.y();
        }
      else
        {
          y = -slack;
          p.y() = pmin.y();
        }
      fastuidraw_gluTessVertex(m_tess, x, y, m_points.fetch(p));
    }
  fastuidraw_gluTessEndContour(m_tess);
}

void
tesser::
add_bounding_box_path(const PointHoard::BoundingBoxes &P)
{
  for(PointHoard::BoundingBoxes::const_iterator iter = P.begin(),
        end = P.end(); iter != end; ++iter, ++m_point_count)
    {
      add_bounding_box_path_element(*iter);
    }
}

void
tesser::
add_bounding_box_path_element(const fastuidraw::uvec4 &box)
{
  const unsigned int indices[4] =
    {
      box_min_x_min_y,
      box_min_x_max_y,
      box_max_x_max_y,
      box_max_x_min_y,
    };

  /* we add the box but tell GLU-tess that the edge does
     not affect winding counts.
     - for each coordinate seperately, for max side: add m_fudge
     - for each coordinate seperately, for max side: subtract m_fudge
  */
  fastuidraw_gluTessBeginContour(m_tess, FASTUIDRAW_GLU_FALSE);
  for(unsigned int i = 0; i < 4; ++i)
    {
      unsigned int k;
      fastuidraw::vecN<double, 2> p;
      double slack;

      k = indices[i];
      p = m_points.converter().apply(m_points[box[k]], 0u);
      slack = static_cast<double>(m_point_count) * m_points.converter().fudge_delta();

      if(k & box_max_x_flag)
        {
          p.x() += slack;
        }
      else
        {
          p.x() -= slack;
        }

      if(k & box_max_y_flag)
        {
          p.y() += slack;
        }
      else
        {
          p.y() -= slack;
        }
      fastuidraw_gluTessVertex(m_tess, p.x(), p.y(), box[k]);
    }
  fastuidraw_gluTessEndContour(m_tess);
}

unsigned int
tesser::
add_point_to_store(const fastuidraw::vec2 &p)
{
  unsigned int return_value;
  return_value = m_points.fetch(p);
  return return_value;
}

bool
tesser::
temp_verts_non_degenerate_triangle(void)
{
  if(m_temp_verts[0] == m_temp_verts[1]
     || m_temp_verts[0] == m_temp_verts[2]
     || m_temp_verts[1] == m_temp_verts[2])
    {
      return false;
    }

  fastuidraw::vec2 p0(m_points[m_temp_verts[0]]);
  fastuidraw::vec2 p1(m_points[m_temp_verts[1]]);
  fastuidraw::vec2 p2(m_points[m_temp_verts[2]]);

  if(p0 == p1 || p0 == p2 || p1 == p2)
    {
      return false;
    }

  fastuidraw::vec2 v(p1 - p0), w(p2 - p0);
  float area;
  bool return_value;

  /* we only reject a triangle if its area to floating
     point arithematic is zero.
   */
  area = fastuidraw::t_abs(v.x() * w.y() - v.y() * w.x());
  return_value = (area > 0.0f);
  return return_value;
}

void
tesser::
begin_callBack(FASTUIDRAW_GLUenum type, int winding_number, void *tess)
{
  tesser *p;
  p = static_cast<tesser*>(tess);
  assert(FASTUIDRAW_GLU_TRIANGLES == type);
  FASTUIDRAWunused(type);

  p->m_temp_vert_count = 0;
  p->on_begin_polygon(winding_number);
}

void
tesser::
vertex_callBack(unsigned int vertex_id, void *tess)
{
  tesser *p;
  p = static_cast<tesser*>(tess);

  if(vertex_id == FASTUIDRAW_GLU_NULL_CLIENT_ID)
    {
      p->m_triangulation_failed = true;
    }

  /* Cache adds vertices in groups of 3 (triangles),
     then if all vertices are NOT FASTUIDRAW_GLU_NULL_CLIENT_ID,
     then add them.
   */
  p->m_temp_verts[p->m_temp_vert_count] = vertex_id;
  p->m_temp_vert_count++;
  if(p->m_temp_vert_count == 3)
    {
      p->m_temp_vert_count = 0;
      /*
        if vertex_id is FASTUIDRAW_GLU_NULL_CLIENT_ID, that means
        the triangle is junked.
      */
      if(p->m_temp_verts[0] != FASTUIDRAW_GLU_NULL_CLIENT_ID
         && p->m_temp_verts[1] != FASTUIDRAW_GLU_NULL_CLIENT_ID
         && p->m_temp_verts[2] != FASTUIDRAW_GLU_NULL_CLIENT_ID
         && p->temp_verts_non_degenerate_triangle())
        {
          fastuidraw::vec2 p0(p->m_points[p->m_temp_verts[0]]);
          fastuidraw::vec2 p1(p->m_points[p->m_temp_verts[1]]);
          fastuidraw::vec2 p2(p->m_points[p->m_temp_verts[2]]);
          fastuidraw::vec2 m01, m02, m12, c;
          unsigned int i01, i02, i12, ic;

          m01 = 0.5f * (p0 + p1);
          m02 = 0.5f * (p0 + p2);
          m12 = 0.5f * (p1 + p2);
          c = (p0 + p1 + p2) / 3.0f;

          i01 = p->m_points.fetch(m01);
          i02 = p->m_points.fetch(m02);
          i12 = p->m_points.fetch(m12);
          ic = p->m_points.fetch(c);

          /* add 6 triangles:
              [p0, m01, c]
              [p0, m02, c]
              [m01, p1, c]
              [c, p1, m12]
              [m02, c, p2]
              [c, m12, p2]

             These 6 triangles are added to guarnantee
             that the all of the interior of a marked
             triangle should have that the coverage
             is non-zero even if all the original vertices
             with which it shares are with triangles from
             another winding number.
           */
          p->add_triangle(p->m_temp_verts[0], i01, ic);
          p->add_triangle(p->m_temp_verts[0], ic, i02);
          p->add_triangle(ic, p->m_temp_verts[1], i12);
          p->add_triangle(i01, p->m_temp_verts[1], ic);
          p->add_triangle(i02, ic, p->m_temp_verts[2]);
          p->add_triangle(ic, i12, p->m_temp_verts[2]);
        }
    }
}

void
tesser::
combine_callback(double x, double y, unsigned int data[4],
                 double weight[4],  unsigned int *outData,
                 void *tess)
{
  FASTUIDRAWunused(x);
  FASTUIDRAWunused(y);

  tesser *p;
  unsigned int v;
  fastuidraw::vec2 pt(0.0f, 0.0f);

  p = static_cast<tesser*>(tess);
  for(unsigned int i = 0; i < 4; ++i)
    {
      if(data[i] != FASTUIDRAW_GLU_NULL_CLIENT_ID)
        {
          pt += float(weight[i]) * p->m_points[data[i]];
        }
    }
  v = p->add_point_to_store(pt);
  *outData = v;
}

FASTUIDRAW_GLUboolean
tesser::
winding_callBack(int winding_number, void *tess)
{
  tesser *p;
  FASTUIDRAW_GLUboolean return_value;

  p = static_cast<tesser*>(tess);
  return_value = p->fill_region(winding_number);
  return return_value;
}

///////////////////////////////////
// non_zero_tesser methods
non_zero_tesser::
non_zero_tesser(PointHoard &points,
                const PointHoard::Path &P,
                const PointHoard::BoundingBoxes &boxes,
                const SubPath &path,
                winding_index_hoard &hoard):
  tesser(points),
  m_winding_start(path.winding_start()),
  m_hoard(hoard),
  m_current_winding(0)
{
  start();
  add_path(P);
  add_bounding_box_path(boxes);
  stop();
}

void
non_zero_tesser::
on_begin_polygon(int winding_number)
{
  winding_number += m_winding_start;
  if(!m_current_indices || m_current_winding != winding_number)
    {
      fastuidraw::reference_counted_ptr<per_winding_data> &h(m_hoard[winding_number]);
      m_current_winding = winding_number;
      if(!h)
        {
          h = FASTUIDRAWnew per_winding_data(winding_number);
        }
      m_current_indices = h;
    }
}

void
non_zero_tesser::
add_vertex_to_polygon(unsigned int vertex)
{
  m_current_indices->add_index(vertex);
  add_to_winding_set(vertex, m_current_indices->winding_number());
}


FASTUIDRAW_GLUboolean
non_zero_tesser::
fill_region(int winding_number)
{
  return winding_number != 0 ?
    FASTUIDRAW_GLU_TRUE :
    FASTUIDRAW_GLU_FALSE;
}

///////////////////////////////
// zero_tesser methods
zero_tesser::
zero_tesser(PointHoard &points,
            const PointHoard::Path &P,
            const PointHoard::BoundingBoxes &boxes,
            const SubPath &path,
            winding_index_hoard &hoard):
  tesser(points),
  m_indices(hoard[path.winding_start()])
{
  if(!m_indices)
    {
      m_indices = FASTUIDRAWnew per_winding_data(path.winding_start());
    }

  start();
  add_path(P);
  add_bounding_box_path(boxes);
  add_path_boundary(path);
  stop();
}

void
zero_tesser::
on_begin_polygon(int winding_number)
{
  assert(winding_number == -1);
  FASTUIDRAWunused(winding_number);
}

void
zero_tesser::
add_vertex_to_polygon(unsigned int vertex)
{
  m_indices->add_index(vertex);
  add_to_winding_set(vertex, m_indices->winding_number());
}

FASTUIDRAW_GLUboolean
zero_tesser::
fill_region(int winding_number)
{
  return winding_number == -1 ?
    FASTUIDRAW_GLU_TRUE :
    FASTUIDRAW_GLU_FALSE;
}

/////////////////////////////////////////
// builder methods
builder::
builder(const SubPath &P, std::vector<FillPoint> &points):
  m_points(P.bounds(), points)
{
  bool failZ, failNZ;
  PointHoard::Path path;
  PointHoard::BoundingBoxes path_bounding_boxes;

  m_points.generate_path(P, path, path_bounding_boxes);
  failNZ = non_zero_tesser::execute_path(m_points, path, path_bounding_boxes, P, m_hoard);
  failZ = zero_tesser::execute_path(m_points, path, path_bounding_boxes, P, m_hoard);
  m_failed= failNZ || failZ;
}

builder::
~builder()
{
}

void
builder::
fill_indices(std::vector<unsigned int> &indices,
             std::map<int, fastuidraw::const_c_array<unsigned int> > &winding_map,
             unsigned int &even_non_zero_start,
             unsigned int &zero_start)
{
  winding_index_hoard::iterator iter, end;
  unsigned int total(0), num_odd(0), num_even_non_zero(0), num_zero(0);

  /* compute number indices needed */
  for(iter = m_hoard.begin(), end = m_hoard.end(); iter != end; ++iter)
    {
      unsigned int cnt;

      cnt = iter->second->count();
      total += cnt;
      if(iter->first == 0)
        {
          num_zero += cnt;
        }
      else if (is_even(iter->first))
        {
          num_even_non_zero += cnt;
        }
      else
        {
          num_odd += cnt;
        }
    }

  /* pack as follows:
      - odd
      - even non-zero
      - zero
   */
  unsigned int current_odd(0), current_even_non_zero(num_odd);
  unsigned int current_zero(num_even_non_zero + num_odd);

  indices.resize(total);
  for(iter = m_hoard.begin(), end = m_hoard.end(); iter != end; ++iter)
    {
      if(iter->first == 0)
        {
          if(iter->second->count() > 0)
            {
              iter->second->fill_at(current_zero,
                                    fastuidraw::make_c_array(indices),
                                    winding_map[iter->first]);
            }
        }
      else if(is_even(iter->first))
        {
          if(iter->second->count() > 0)
            {
              iter->second->fill_at(current_even_non_zero,
                                    fastuidraw::make_c_array(indices),
                                    winding_map[iter->first]);
            }
        }
      else
        {
          if(iter->second->count() > 0)
            {
              iter->second->fill_at(current_odd,
                                    fastuidraw::make_c_array(indices),
                                    winding_map[iter->first]);
            }
        }
    }

  assert(current_zero == total);
  assert(current_odd == num_odd);
  assert(current_even_non_zero == current_odd + num_even_non_zero);

  even_non_zero_start = num_odd;
  zero_start = current_odd + num_even_non_zero;

}

////////////////////////////////
// AttributeDataMerger methods
void
AttributeDataMerger::
fill_winding_data(const std::vector<WindingSet> &a,
                  const std::vector<WindingSet> &b,
                  std::vector<WindingSet> &dst)
{
  unsigned int a_size(a.size()), b_size(b.size());

  dst.resize(a_size + b_size);
  std::copy(a.begin(), a.end(), dst.begin());
  std::copy(b.begin(), b.end(), dst.begin() + a_size);
}

void
AttributeDataMerger::
compute_sizes(unsigned int &number_attributes,
              unsigned int &number_indices,
              unsigned int &number_attribute_chunks,
              unsigned int &number_index_chunks,
              unsigned int &number_z_increments) const
{
  number_z_increments = 0;
  number_attributes = m_a.attribute_data_chunk(0).size() + m_b.attribute_data_chunk(0).size();
  number_attribute_chunks = 1;
  number_index_chunks = fastuidraw::t_max(m_a.index_data_chunks().size(),
                                          m_b.index_data_chunks().size());
  number_indices = 0;
  for(unsigned int c = 0; c < number_index_chunks; ++c)
    {
      unsigned int a_sz, b_sz;

      a_sz = m_a.index_data_chunk(c).size();
      b_sz = m_b.index_data_chunk(c).size();
      number_indices += (a_sz + b_sz);
    }
}

void
AttributeDataMerger::
fill_data(fastuidraw::c_array<fastuidraw::PainterAttribute> attributes,
          fastuidraw::c_array<fastuidraw::PainterIndex> indices,
          fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterAttribute> > attrib_chunks,
          fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterIndex> > index_chunks,
          fastuidraw::c_array<unsigned int> zincrements,
          fastuidraw::c_array<int> index_adjusts) const
{
  fastuidraw::c_array<fastuidraw::PainterAttribute> a_attribs, b_attribs;

  FASTUIDRAWunused(zincrements);

  a_attribs = attributes.sub_array(0, m_a.attribute_data_chunk(0).size());
  b_attribs = attributes.sub_array(m_a.attribute_data_chunk(0).size());
  assert(b_attribs.size() == m_b.attribute_data_chunk(0).size());

  attrib_chunks[0] = attributes;
  /* copy attributes with attributes of m_a first
   */
  std::copy(m_a.attribute_data_chunk(0).begin(),
            m_a.attribute_data_chunk(0).end(),
            a_attribs.begin());

  std::copy(m_b.attribute_data_chunk(0).begin(),
            m_b.attribute_data_chunk(0).end(),
            b_attribs.begin());

  /* copy indices is trickier; we need to copy with correct chunking
     AND adjust the values for the indices coming from m_b (because
     m_b attributes are placed after m_a attributes).
   */
  for(unsigned int chunk = 0, end_chunk = index_chunks.size(), current = 0; chunk < end_chunk; ++chunk)
    {
      fastuidraw::c_array<fastuidraw::PainterIndex> dst, dst_a, dst_b;
      unsigned int dst_size, a_sz, b_sz;

      index_adjusts[chunk] = 0;

      a_sz = m_a.index_data_chunk(chunk).size();
      b_sz = m_b.index_data_chunk(chunk).size();
      dst_size = a_sz + b_sz;

      dst = indices.sub_array(current, dst_size);
      index_chunks[chunk] = dst;
      dst_a = dst.sub_array(0, a_sz);
      dst_b = dst.sub_array(a_sz);
      current += dst_size;

      if(a_sz > 0)
        {
          std::copy(m_a.index_data_chunk(chunk).begin(),
                    m_a.index_data_chunk(chunk).end(),
                    dst_a.begin());
        }

      if(b_sz > 0)
        {
          fastuidraw::const_c_array<fastuidraw::PainterIndex> src;

          src = m_b.index_data_chunk(chunk);
          for(unsigned int i = 0; i < b_sz; ++i)
            {
              dst_b[i] = src[i] + a_attribs.size();
            }
        }
    }
}

////////////////////////////////////
// AttributeDataFiller methods
void
AttributeDataFiller::
compute_sizes(unsigned int &number_attributes,
              unsigned int &number_indices,
              unsigned int &number_attribute_chunks,
              unsigned int &number_index_chunks,
              unsigned int &number_z_increments) const
{
  using namespace fastuidraw;

  number_z_increments = 0;
  if(m_per_fill.empty())
    {
      number_attributes = 0;
      number_indices = 0;
      number_attribute_chunks = 0;
      number_index_chunks = 0;
      return;
    }
  number_attributes = m_points.size();
  number_attribute_chunks = 1;

  number_indices = m_odd_winding_indices.size()
    + m_nonzero_winding_indices.size()
    + m_even_winding_indices.size()
    + m_zero_winding_indices.size();

  for(std::map<int, const_c_array<unsigned int> >::const_iterator
        iter = m_per_fill.begin(), end = m_per_fill.end();
      iter != end; ++iter)
    {
      if(iter->first != 0) //winding number 0 is by complement_nonzero_fill_rule
        {
          number_indices += iter->second.size();
        }
    }

  /* now get how big the index_chunks really needs to be
   */
  int smallest_winding(m_per_fill.begin()->first);
  int largest_winding(m_per_fill.rbegin()->first);
  unsigned int largest_winding_idx(FilledPath::Subset::chunk_from_winding_number(largest_winding));
  unsigned int smallest_winding_idx(FilledPath::Subset::chunk_from_winding_number(smallest_winding));
  number_index_chunks = 1 + std::max(largest_winding_idx, smallest_winding_idx);
}

void
AttributeDataFiller::
fill_data(fastuidraw::c_array<fastuidraw::PainterAttribute> attributes,
          fastuidraw::c_array<fastuidraw::PainterIndex> index_data,
          fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterAttribute> > attrib_chunks,
          fastuidraw::c_array<fastuidraw::const_c_array<fastuidraw::PainterIndex> > index_chunks,
          fastuidraw::c_array<unsigned int> zincrements,
          fastuidraw::c_array<int> index_adjusts) const
{
  using namespace fastuidraw;

  if(m_per_fill.empty())
    {
      return;
    }
  assert(attributes.size() == m_points.size());
  assert(attrib_chunks.size() == 1);
  assert(zincrements.empty());
  FASTUIDRAWunused(zincrements);

  /* generate attribute data
   */
  std::transform(m_points.begin(), m_points.end(), attributes.begin(),
                 AttributeDataFiller::generate_attribute);
  attrib_chunks[0] = attributes;
  std::fill(index_adjusts.begin(), index_adjusts.end(), 0);

  unsigned int current(0);

#define GRAB_MACRO(enum_name, member_name) do {                     \
    c_array<PainterIndex> dst;                                      \
    dst = index_data.sub_array(current, member_name.size());        \
    std::copy(member_name.begin(),                                  \
              member_name.end(), dst.begin());                      \
    index_chunks[PainterEnums::enum_name] = dst;                    \
    current += dst.size();                                          \
  } while(0)

  GRAB_MACRO(odd_even_fill_rule, m_odd_winding_indices);
  GRAB_MACRO(nonzero_fill_rule, m_nonzero_winding_indices);
  GRAB_MACRO(complement_odd_even_fill_rule, m_even_winding_indices);
  GRAB_MACRO(complement_nonzero_fill_rule, m_zero_winding_indices);

#undef GRAB_MACRO

  for(std::map<int, const_c_array<unsigned int> >::const_iterator
        iter = m_per_fill.begin(), end = m_per_fill.end();
      iter != end; ++iter)
    {
      if(iter->first != 0) //winding number 0 is by complement_nonzero_fill_rule
        {
          c_array<PainterIndex> dst;
          const_c_array<unsigned int> src;
          unsigned int idx;

          idx = FilledPath::Subset::chunk_from_winding_number(iter->first);

          src = iter->second;
          dst = index_data.sub_array(current, src.size());
          assert(dst.size() == src.size());

          std::copy(src.begin(), src.end(), dst.begin());

          index_chunks[idx] = dst;
          current += dst.size();
        }
    }
}

void
AttributeDataFiller::
fill_winding_data(std::vector<WindingSet> &dst) const
{
  dst.resize(m_points.size());
  for(unsigned int v = 0, endv = dst.size(); v < endv; ++v)
    {
      dst[v].extract_from_set(m_points[v].m_winding);
    }
}

//////////////////////////////////
// WindingSet methods
void
WindingSet::
extract_from_fill_fule(int min_value, int max_value,
                       const fastuidraw::CustomFillRuleBase &fill_rule,
                       bool flip)
{
  m_begin = min_value;
  m_end = max_value + 1;
  assert(m_begin <= m_end);

  m_bits.resize(m_end - m_begin, false);
  for(int w = m_begin; w < m_end; ++w)
    {
      m_bits.set(w - m_begin, fill_rule(w) != flip);
    }
}

void
WindingSet::
extract_from_set(const std::set<int> &in_values)
{
  m_bits.clear();
  if(in_values.empty())
    {
      m_begin = m_end = 0;
      return;
    }

  m_begin = *in_values.begin();
  m_end = *in_values.rbegin() + 1;
  m_bits.resize(m_end - m_begin, false);
  for(std::set<int>::const_iterator iter = in_values.begin(),
        end = in_values.end(); iter != end; ++iter)
    {
      m_bits.set(*iter - m_begin, true);
    }
}

bool
WindingSet::
have_common_bit(const WindingSet &obj) const
{
  for(int w = fastuidraw::t_max(m_begin, obj.m_begin),
        endw = fastuidraw::t_min(m_end, obj.m_end);
      w < endw; ++w)
    {
      if(m_bits.test(w - m_begin) && obj.m_bits.test(w - obj.m_begin))
        {
          return true;
        }
    }
  return false;
}

bool
WindingSet::
has(int w) const
{
  assert(w < m_begin || w >= m_end || (w - m_begin) <= static_cast<int>(m_bits.size()));
  return (w >= m_begin && w < m_end) ?
    m_bits.test(w - m_begin) : false;
}

/////////////////////////////////
// SubsetPrivate methods
SubsetPrivate::
SubsetPrivate(SubPath *Q, int max_recursion,
              std::vector<SubsetPrivate*> &out_values):
  m_ID(out_values.size()),
  m_bounds(Q->bounds()),
  m_painter_data(NULL),
  m_sizes_ready(false),
  m_sub_path(Q),
  m_children(NULL, NULL)
{
  out_values.push_back(this);
  if(max_recursion > 0 && m_sub_path->total_points() > SubsetConstants::points_per_subset)
    {
      fastuidraw::vecN<SubPath*, 2> C;

      C = Q->split();
      if(C[0]->total_points() < m_sub_path->total_points() || C[1]->total_points() < m_sub_path->total_points())
        {
          m_children[0] = FASTUIDRAWnew SubsetPrivate(C[0], max_recursion - 1, out_values);
          m_children[1] = FASTUIDRAWnew SubsetPrivate(C[1], max_recursion - 1, out_values);
          FASTUIDRAWdelete(m_sub_path);
          m_sub_path = NULL;
        }
      else
        {
          FASTUIDRAWdelete(C[0]);
          FASTUIDRAWdelete(C[1]);
        }
    }
}

SubsetPrivate::
~SubsetPrivate(void)
{
  if(m_sub_path != NULL)
    {
      assert(m_painter_data == NULL);
      assert(m_children[0] == NULL);
      assert(m_children[1] == NULL);
      FASTUIDRAWdelete(m_sub_path);
    }

  if(m_painter_data != NULL)
    {
      assert(m_sub_path == NULL);
      FASTUIDRAWdelete(m_painter_data);
    }

  if(m_children[0] != NULL)
    {
      assert(m_sub_path == NULL);
      assert(m_children[1] != NULL);
      FASTUIDRAWdelete(m_children[0]);
      FASTUIDRAWdelete(m_children[1]);
    }
}

unsigned int
SubsetPrivate::
select_subsets(ScratchSpacePrivate &scratch,
               fastuidraw::const_c_array<fastuidraw::vec3> clip_equations,
               const fastuidraw::float3x3 &clip_matrix_local,
               unsigned int max_attribute_cnt,
               unsigned int max_index_cnt,
               fastuidraw::c_array<unsigned int> dst)
{
  unsigned int return_value(0u);

  scratch.m_adjusted_clip_eqs.resize(clip_equations.size());
  for(unsigned int i = 0; i < clip_equations.size(); ++i)
    {
      /* transform clip equations from clip coordinates to
         local coordinates.
       */
      scratch.m_adjusted_clip_eqs[i] = clip_equations[i] * clip_matrix_local;
    }

  select_subsets_implement(scratch, dst, max_attribute_cnt, max_index_cnt, return_value);
  return return_value;
}

void
SubsetPrivate::
select_subsets_implement(ScratchSpacePrivate &scratch,
                         fastuidraw::c_array<unsigned int> dst,
                         unsigned int max_attribute_cnt,
                         unsigned int max_index_cnt,
                         unsigned int &current)
{
  using namespace fastuidraw;
  using namespace fastuidraw::detail;

  vecN<vec2, 4> bb;
  bool unclipped;

  m_bounds.inflated_polygon(bb, 0.0f);
  unclipped = clip_against_planes(make_c_array(scratch.m_adjusted_clip_eqs),
                                  bb, scratch.m_clipped_rect,
                                  scratch.m_clip_scratch_floats,
                                  scratch.m_clip_scratch_vec2s);

  //completely clipped
  if(scratch.m_clipped_rect.empty())
    {
      return;
    }

  //completely unclipped or no children
  assert((m_children[0] == NULL) == (m_children[1] == NULL));
  if(unclipped || m_children[0] == NULL)
    {
      select_subsets_all_unculled(dst, max_attribute_cnt, max_index_cnt, current);
      return;
    }

  m_children[0]->select_subsets_implement(scratch, dst, max_attribute_cnt, max_index_cnt, current);
  m_children[1]->select_subsets_implement(scratch, dst, max_attribute_cnt, max_index_cnt, current);
}

void
SubsetPrivate::
select_subsets_all_unculled(fastuidraw::c_array<unsigned int> dst,
                            unsigned int max_attribute_cnt,
                            unsigned int max_index_cnt,
                            unsigned int &current)
{
  if(!m_sizes_ready && m_children[0] == NULL)
    {
      /* we are going to need the attributes because
         the element will be selected.
       */
      make_ready_from_sub_path();
      assert(m_painter_data != NULL);
    }

  if(m_sizes_ready && m_num_attributes <= max_attribute_cnt && m_largest_index_block <= max_index_cnt)
    {
      dst[current] = m_ID;
      ++current;
    }
  else if(m_children[0] != NULL)
    {
      m_children[0]->select_subsets_all_unculled(dst, max_attribute_cnt, max_index_cnt, current);
      m_children[1]->select_subsets_all_unculled(dst, max_attribute_cnt, max_index_cnt, current);
      if(!m_sizes_ready)
        {
          m_sizes_ready = true;
          assert(m_children[0]->m_sizes_ready);
          assert(m_children[1]->m_sizes_ready);
          m_num_attributes = m_children[0]->m_num_attributes + m_children[1]->m_num_attributes;
          m_largest_index_block = m_children[0]->m_largest_index_block + m_children[1]->m_largest_index_block;
        }
    }
  else
    {
      assert(m_sizes_ready);
      assert(!"Childless FilledPath::Subset has too many attributes or indices");
    }
}

void
SubsetPrivate::
make_ready(void)
{
  if(m_painter_data == NULL)
    {
      if(m_sub_path != NULL)
        {
          make_ready_from_sub_path();
        }
      else
        {
          make_ready_from_children();
        }
    }
}

void
SubsetPrivate::
make_ready_from_children(void)
{
  assert(m_children[0] != NULL);
  assert(m_children[1] != NULL);
  assert(m_sub_path == NULL);
  assert(m_painter_data == NULL);

  m_children[0]->make_ready();
  m_children[1]->make_ready();

  AttributeDataMerger merger(m_children[0]->painter_data(),
                             m_children[1]->painter_data());
  std::set<int> wnd;

  m_painter_data = FASTUIDRAWnew fastuidraw::PainterAttributeData();
  m_painter_data->set_data(merger);
  AttributeDataMerger::fill_winding_data(m_children[0]->windings_per_pt(),
                                         m_children[1]->windings_per_pt(),
                                         m_windings_per_pt);

  std::copy(m_children[0]->winding_numbers().begin(),
            m_children[0]->winding_numbers().end(),
            std::inserter(wnd, wnd.begin()));
  std::copy(m_children[1]->winding_numbers().begin(),
            m_children[1]->winding_numbers().end(),
            std::inserter(wnd, wnd.begin()));
  m_winding_numbers.resize(wnd.size());
  std::copy(wnd.begin(), wnd.end(), m_winding_numbers.begin());

  if(!m_sizes_ready)
    {
      m_sizes_ready = true;
      assert(m_children[0]->m_sizes_ready);
      assert(m_children[1]->m_sizes_ready);
      m_num_attributes = m_children[0]->m_num_attributes + m_children[1]->m_num_attributes;
      m_largest_index_block = m_children[0]->m_largest_index_block + m_children[1]->m_largest_index_block;
    }
}

void
SubsetPrivate::
make_ready_from_sub_path(void)
{
  assert(m_children[0] == NULL);
  assert(m_children[1] == NULL);
  assert(m_sub_path != NULL);
  assert(m_painter_data == NULL);
  assert(!m_sizes_ready);

  AttributeDataFiller filler;
  builder B(*m_sub_path, filler.m_points);
  unsigned int even_non_zero_start, zero_start;
  unsigned int m1, m2;

  B.fill_indices(filler.m_indices, filler.m_per_fill, even_non_zero_start, zero_start);

  fastuidraw::const_c_array<unsigned int> indices_ptr;
  indices_ptr = fastuidraw::make_c_array(filler.m_indices);
  filler.m_nonzero_winding_indices = indices_ptr.sub_array(0, zero_start);
  filler.m_odd_winding_indices = indices_ptr.sub_array(0, even_non_zero_start);
  filler.m_even_winding_indices = indices_ptr.sub_array(even_non_zero_start);
  filler.m_zero_winding_indices = indices_ptr.sub_array(zero_start);

  m_sizes_ready = true;
  m1 = fastuidraw::t_max(filler.m_nonzero_winding_indices.size(),
                         filler.m_zero_winding_indices.size());
  m2 = fastuidraw::t_max(filler.m_odd_winding_indices.size(),
                         filler.m_even_winding_indices.size());
  m_largest_index_block = fastuidraw::t_max(m1, m2);
  m_num_attributes = filler.m_points.size();

  m_winding_numbers.reserve(filler.m_per_fill.size());
  for(std::map<int, fastuidraw::const_c_array<unsigned int> >::iterator
        iter = filler.m_per_fill.begin(), end = filler.m_per_fill.end();
      iter != end; ++iter)
    {
      assert(!iter->second.empty());
      m_winding_numbers.push_back(iter->first);
    }

  /* now fill m_painter_data.
   */
  m_painter_data = FASTUIDRAWnew fastuidraw::PainterAttributeData();
  m_painter_data->set_data(filler);

  filler.fill_winding_data(m_windings_per_pt);

  FASTUIDRAWdelete(m_sub_path);
  m_sub_path = NULL;

  #ifdef FASTUIDRAW_DEBUG
    {
      if(B.triangulation_failed())
        {
          /* On debug builds, print a warning.
           */
          std::cerr << "[" << __FILE__ << ", " << __LINE__
                    << "] Triangulation failed on tessellated path "
                    << this << "\n";
        }
    }
  #endif

}

/////////////////////////////////
// FilledPathPrivate methods
FilledPathPrivate::
FilledPathPrivate(const fastuidraw::TessellatedPath &P)
{
  SubPath *q;
  q = FASTUIDRAWnew SubPath(P);
  m_root = FASTUIDRAWnew SubsetPrivate(q, SubsetConstants::recursion_depth, m_subsets);
}

FilledPathPrivate::
~FilledPathPrivate()
{
  FASTUIDRAWdelete(m_root);
}

///////////////////////////////
//fastuidraw::FilledPath::ScratchSpace methods
fastuidraw::FilledPath::ScratchSpace::
ScratchSpace(void)
{
  m_d = FASTUIDRAWnew ScratchSpacePrivate();
}

fastuidraw::FilledPath::ScratchSpace::
~ScratchSpace(void)
{
  ScratchSpacePrivate *d;
  d = reinterpret_cast<ScratchSpacePrivate*>(m_d);
  FASTUIDRAWdelete(d);
  m_d = NULL;
}

/////////////////////////////////////////////
// fastuidraw::FilledPath::DataWriter methods
fastuidraw::FilledPath::DataWriter::
DataWriter(void)
{
  m_d = FASTUIDRAWnew DataWriterPrivate();
}

fastuidraw::FilledPath::DataWriter::
DataWriter(const DataWriter &obj)
{
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(obj.m_d);
  m_d = FASTUIDRAWnew DataWriterPrivate(*d);
}

fastuidraw::FilledPath::DataWriter::
~DataWriter(void)
{
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(m_d);
  FASTUIDRAWdelete(d);
  m_d = NULL;
}

const fastuidraw::FilledPath::DataWriter&
fastuidraw::FilledPath::DataWriter::
operator=(const DataWriter &rhs)
{
  if(&rhs != this)
    {
      DataWriter tmp(rhs);
      swap(tmp);
    }
  return *this;
}

void
fastuidraw::FilledPath::DataWriter::
swap(DataWriter &obj)
{
  std::swap(obj.m_d, m_d);
}

unsigned int
fastuidraw::FilledPath::DataWriter::
number_attribute_chunks(void) const
{
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(m_d);
  return d->m_attribute_chunks.size();
}

unsigned int
fastuidraw::FilledPath::DataWriter::
number_attributes(unsigned int attribute_chunk) const
{
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(m_d);
  assert(attribute_chunk < d->m_attribute_chunks.size());
  return d->m_attribute_chunks[attribute_chunk].m_attribs.size();
}

unsigned int
fastuidraw::FilledPath::DataWriter::
number_index_chunks(void) const
{
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(m_d);
  return d->m_index_chunks.size();
}

unsigned int
fastuidraw::FilledPath::DataWriter::
number_indices(unsigned int index_chunk) const
{
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(m_d);
  assert(index_chunk < d->m_index_chunks.size());
  return d->m_index_chunks[index_chunk].m_indices.size();
}

unsigned int
fastuidraw::FilledPath::DataWriter::
attribute_chunk_selection(unsigned int index_chunk) const
{
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(m_d);
  assert(index_chunk < d->m_index_chunks.size());
  return d->m_index_chunks[index_chunk].m_attrib_chunk;
}

void
fastuidraw::FilledPath::DataWriter::
write_indices(c_array<PainterIndex> dst,
              unsigned int index_offset_value,
              unsigned int index_chunk) const
{
  const_c_array<PainterIndex> src;
  DataWriterPrivate *d;
  d = reinterpret_cast<DataWriterPrivate*>(m_d);

  assert(index_chunk < d->m_index_chunks.size());
  src = d->m_index_chunks[index_chunk].m_indices;

  assert(dst.size() == src.size());
  for(unsigned int i = 0; i < dst.size(); ++i)
    {
      dst[i] = src[i] + index_offset_value;
    }
}

void
fastuidraw::FilledPath::DataWriter::
write_attributes(c_array<PainterAttribute> dst,
                 unsigned int attribute_chunk) const
{
  const_c_array<PainterAttribute> src;
  const_c_array<WindingSet> w_src;
  DataWriterPrivate *d;

  d = reinterpret_cast<DataWriterPrivate*>(m_d);

  assert(attribute_chunk < d->m_attribute_chunks.size());
  src = d->m_attribute_chunks[attribute_chunk].m_attribs;
  w_src = d->m_attribute_chunks[attribute_chunk].m_per_pt_winding_set;

  assert(dst.size() == src.size());
  for(unsigned int i = 0; i < dst.size(); ++i)
    {
      bool outside;
      float value;

      /* each attribute v has a bitset giving the set
         S(v) that is the set of all winding numbers w
         for which there is a triangle T which uses v
         as a vertex and whose winding number is w.

         A vertex v is on the boundary if there is a
         value w of S(v) which is not to be filled.
      */
      dst[i].m_attrib0.x() = src[i].m_attrib0.x();
      dst[i].m_attrib0.y() = src[i].m_attrib0.y();

      outside = d->m_complement_winding_rule.have_common_bit(w_src[i]);
      value = (outside) ? 0.0f: 1.0f;
      dst[i].m_attrib0.z() = pack_float(value);
    }
}

/////////////////////////////////
// fastuidraw::FilledPath::Subset methods
fastuidraw::FilledPath::Subset::
Subset(void *d):
  m_d(d)
{
}

const fastuidraw::PainterAttributeData&
fastuidraw::FilledPath::Subset::
painter_data(void) const
{
  SubsetPrivate *d;
  d = static_cast<SubsetPrivate*>(m_d);
  return d->painter_data();
}

fastuidraw::const_c_array<int>
fastuidraw::FilledPath::Subset::
winding_numbers(void) const
{
  SubsetPrivate *d;
  d = static_cast<SubsetPrivate*>(m_d);
  return d->winding_numbers();
}

unsigned int
fastuidraw::FilledPath::Subset::
chunk_from_winding_number(int winding_number)
{
  /* basic idea:
     - start counting at fill_rule_data_count
     - ordering is: 1, -1, 2, -2, ...
  */
  int value, sg;

  if(winding_number == 0)
    {
      return fastuidraw::PainterEnums::complement_nonzero_fill_rule;
    }

  value = std::abs(winding_number);
  sg = (winding_number < 0) ? 1 : 0;
  return fastuidraw::PainterEnums::fill_rule_data_count + sg + 2 * (value - 1);
}

unsigned int
fastuidraw::FilledPath::Subset::
chunk_from_fill_rule(enum PainterEnums::fill_rule_t fill_rule)
{
  assert(fill_rule < fastuidraw::PainterEnums::fill_rule_data_count);
  return fill_rule;
}

///////////////////////////////////////
// fastuidraw::FilledPath methods
fastuidraw::FilledPath::
FilledPath(const TessellatedPath &P)
{
  m_d = FASTUIDRAWnew FilledPathPrivate(P);
}

fastuidraw::FilledPath::
~FilledPath()
{
  FilledPathPrivate *d;
  d = reinterpret_cast<FilledPathPrivate*>(m_d);
  FASTUIDRAWdelete(d);
  m_d = NULL;
}

unsigned int
fastuidraw::FilledPath::
number_subsets(void) const
{
  FilledPathPrivate *d;
  d = reinterpret_cast<FilledPathPrivate*>(m_d);
  return d->m_subsets.size();
}


fastuidraw::FilledPath::Subset
fastuidraw::FilledPath::
subset(unsigned int I) const
{
  FilledPathPrivate *d;
  SubsetPrivate *p;

  d = reinterpret_cast<FilledPathPrivate*>(m_d);
  assert(I < d->m_subsets.size());
  p = d->m_subsets[I];
  p->make_ready();

  return Subset(p);
}

unsigned int
fastuidraw::FilledPath::
select_subsets(ScratchSpace &work_room,
               const_c_array<vec3> clip_equations,
               const float3x3 &clip_matrix_local,
               unsigned int max_attribute_cnt,
               unsigned int max_index_cnt,
               c_array<unsigned int> dst) const
{
  FilledPathPrivate *d;
  unsigned int return_value;

  d = reinterpret_cast<FilledPathPrivate*>(m_d);
  assert(dst.size() >= d->m_subsets.size());
  /* TODO:
       - have another method in SubsetPrivate called
         "fast_select_subsets" which ignores the requirements
         coming from max_attribute_cnt and max_index_cnt.
         By ignoring this requirement, we do NOT need
         to do call make_ready() for any SubsetPrivate
         object chosen.
       - have the fast_select_subsets() also return
         if paths needed require triangulation.
       - if there such, spawn a thread and let the
         caller decide if to wait for the thread to
         finish before proceeding or to do something
         else (like use a lower level of detail that
         is ready). Another alternatic is to return
         what Subset's need to have triangulation done
         and spawn a set of threads to do the job (!)
       - All this work means we need to make SubsetPrivate
         thread safe (with regards to the SubsetPrivate
         being made ready via make_ready()).
   */
  return_value = d->m_root->select_subsets(*static_cast<ScratchSpacePrivate*>(work_room.m_d),
                                           clip_equations, clip_matrix_local,
                                           max_attribute_cnt, max_index_cnt, dst);

  return return_value;
}


void
fastuidraw::FilledPath::
compute_writer(ScratchSpace &scratch_space,
               const CustomFillRuleBase &fill_rule,
               const_c_array<vec3> clip_equations,
               const float3x3 &clip_matrix_local,
               unsigned int max_attribute_cnt,
               unsigned int max_index_cnt,
               DataWriter &dst) const
{
  DataWriterPrivate *dst_d;
  unsigned int num;

  dst_d = reinterpret_cast<DataWriterPrivate*>(dst.m_d);

  dst_d->m_attribute_chunks.clear();
  dst_d->m_index_chunks.clear();
  dst_d->m_complement_winding_rule.clear();

  dst_d->m_subset_selector.resize(number_subsets());
  num = select_subsets(scratch_space, clip_equations, clip_matrix_local,
                       max_attribute_cnt, max_index_cnt,
                       make_c_array(dst_d->m_subset_selector));

  if(num == 0)
    {
      return;
    }

  Subset S(subset(dst_d->m_subset_selector[0]));
  int min_winding, max_winding;

  min_winding = S.winding_numbers().front();
  max_winding = S.winding_numbers().back();
  for(unsigned int i = 1; i < num; ++i)
    {
      S = subset(dst_d->m_subset_selector[i]);
      min_winding = t_min(min_winding, S.winding_numbers().front());
      max_winding = t_max(max_winding, S.winding_numbers().back());
    }

  dst_d->m_winding_rule.extract_from_fill_fule(min_winding, max_winding, fill_rule, false);
  dst_d->m_complement_winding_rule.extract_from_fill_fule(min_winding, max_winding, fill_rule, true);
  dst_d->m_attribute_chunks.reserve(num);
  dst_d->m_index_chunks.reserve(num);

  for(unsigned int i = 0; i < num; ++i)
    {
      SubsetPrivate *sd;
      const_c_array<int> windings;
      const unsigned int ATTRIB_CHUNK_NOT_TAKEN = ~0u;
      unsigned int attrib_chunk;

      S = subset(dst_d->m_subset_selector[i]);
      sd = reinterpret_cast<SubsetPrivate*>(S.m_d);
      windings = sd->winding_numbers();
      attrib_chunk = ATTRIB_CHUNK_NOT_TAKEN;

      for(unsigned int i = 0; i < windings.size(); ++i)
        {
          int w;

          w = windings[i];
          if(dst_d->m_winding_rule.has(w))
            {
              unsigned int index_chunk;
              const_c_array<PainterIndex> indices;

              if(attrib_chunk == ATTRIB_CHUNK_NOT_TAKEN)
                {
                  attrib_chunk = dst_d->m_attribute_chunks.size();
                  dst_d->m_attribute_chunks.push_back(DataWriterPrivate::per_attrib_chunk(sd));
                }

              index_chunk = Subset::chunk_from_winding_number(w);
              indices = sd->painter_data().index_data_chunk(index_chunk);

              dst_d->m_index_chunks.push_back(DataWriterPrivate::per_index_chunk(indices, attrib_chunk));
            }
        }
    }

}

void
fastuidraw::FilledPath::
compute_writer(ScratchSpace &scratch_space,
               const PainterEnums::fill_rule_t fill_rule,
               const_c_array<vec3> clip_equations,
               const float3x3 &clip_matrix_local,
               unsigned int max_attribute_cnt,
               unsigned int max_index_cnt,
               DataWriter &dst) const
{
  bool (*fcn)(int);

  switch(fill_rule)
    {
    case PainterEnums::nonzero_fill_rule:
      fcn = fcn_non_zero_fill_rule;
      break;
    case PainterEnums::complement_nonzero_fill_rule:
      fcn = fcn_complelemt_non_zero_fill_rule;
      break;
    case PainterEnums::odd_even_fill_rule:
      fcn = fcn_odd_even_fill_rule;
      break;
    case PainterEnums::complement_odd_even_fill_rule:
      fcn = fcn_complement_odd_even_fill_rule;
      break;

    default:
      assert(!"Invalid fill rule enumeration, using non-zero");
      fcn = fcn_non_zero_fill_rule;
    }

  compute_writer(scratch_space, CustomFillRuleFunction(fcn),
                 clip_equations, clip_matrix_local,
                 max_attribute_cnt, max_index_cnt, dst);
}
