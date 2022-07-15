#ifndef TREE_CRS_POLICY_HPP
#define TREE_CRS_POLICY_HPP

#include "ArborX_LinearBVH.hpp"
#include "Kokkos_Core.hpp"
#include "constants.hpp"
#include "containers.hpp"
#include "type_defs.hpp"

// these are some structs that are necessary for the ArborX fixed-radius search
// this was provided by the helpful ArborX/Kokkos developer dalg24
template <class MemorySpace>
struct Points {
  Kokkos::View<float*, MemorySpace> x;
  Kokkos::View<float*, MemorySpace> y;
  Kokkos::View<float*, MemorySpace> z;
  int N;
  Points<MemorySpace>(const Kokkos::View<Real**>& _X, const int& dim) {
    N = _X.extent(1);
    x = Kokkos::View<float*, MemorySpace>("pts x", N);
    y = Kokkos::View<float*, MemorySpace>("pts y", N);
    z = Kokkos::View<float*, MemorySpace>("pts z", N);
    // FIXME: it'd be cleaner to pre-initialize and do this in a dim-length loop
    if (dim == 1) {
      Kokkos::deep_copy(x, Kokkos::subview(_X, 0, Kokkos::ALL()));
      Kokkos::deep_copy(y, 0.0);
      Kokkos::deep_copy(z, 0.0);
    } else if (dim == 2) {
      Kokkos::deep_copy(x, Kokkos::subview(_X, 0, Kokkos::ALL()));
      Kokkos::deep_copy(y, Kokkos::subview(_X, 1, Kokkos::ALL()));
      Kokkos::deep_copy(z, 0.0);

    } else if (dim == 3) {
      Kokkos::deep_copy(x, Kokkos::subview(_X, 0, Kokkos::ALL()));
      Kokkos::deep_copy(y, Kokkos::subview(_X, 1, Kokkos::ALL()));
      Kokkos::deep_copy(z, Kokkos::subview(_X, 2, Kokkos::ALL()));
    } else {
      printf("Dimensions of X = %i. Why so many?", dim);
      exit(EXIT_FAILURE);
    }
  }
};
template <class MemorySpace>
struct Spheres {
  Points<MemorySpace> points;
  float radius;
};
template <class MemorySpace>
struct ArborX::AccessTraits<Points<MemorySpace>, ArborX::PrimitivesTag> {
  using memory_space = MemorySpace;
  using size_type = std::size_t;
  static KOKKOS_FUNCTION size_type size(Points<MemorySpace> const& points) {
    return points.N;
  }
  static KOKKOS_FUNCTION ArborX::Point get(Points<MemorySpace> const& points,
                                           size_type i) {
    return {{points.x(i), points.y(i), points.z(i)}};
  }
};
template <class MemorySpace>
struct ArborX::AccessTraits<Spheres<MemorySpace>, ArborX::PredicatesTag> {
  using memory_space = MemorySpace;
  using size_type = std::size_t;
  static KOKKOS_FUNCTION size_type size(Spheres<MemorySpace> const& x) {
    return x.points.N;
  }
  static KOKKOS_FUNCTION auto get(Spheres<MemorySpace> const& x, size_type i) {
    return ArborX::attach(
        ArborX::intersects(ArborX::Sphere{
            ArborX::Point{x.points.x(i), x.points.y(i), x.points.z(i)},
            x.radius}),
        (int)i);
  }
};

namespace particles {

struct TreeCRSPolicy {
  static SparseMatViews get_views(const ko::View<Real**>& X,
                                  const Params& params, int& nnz, const int& Nc,
                                  const int& substart, const int& subend) {
    SparseMatViews spmat_views;
    auto ldim = params.dim;
    auto lX = X;
    auto lNp = Nc;
    Real denom = params.denom;
    Real c = pow(denom * pi, (Real)ldim / 2.0);

    // make a float version of radius and X, as required by arborx, being
    // careful because the X view passed to arborx must have dimension 3
    ko::Profiling::pushRegion("create device/float views");
    // Note: this needs to be column-major for some reason, or ArborX messes up
    Points<MemorySpace> fX(lX, ldim);
    auto subX = ko::View<Real**, MemorySpace>("subX", ldim, subend - substart);
    ko::deep_copy(subX, ko::subview(lX, ko::ALL(), ko::make_pair(substart, subend)));
    Points<MemorySpace> fsubX(subX, ldim);
    float frad = float(params.cutdist);
    ko::Profiling::popRegion();

    // create the bounding value hierarchy and query it for the fixed-radius
    // search
    ko::Profiling::pushRegion("create BVH");
    ArborX::BVH<MemorySpace> bvh{ExecutionSpace(), fX};
    ko::Profiling::popRegion();
    ko::Profiling::pushRegion("create offset/indices views");
    spmat_views.rowmap = ko::View<int*, MemorySpace>("rowmap", 0);
    spmat_views.col = ko::View<int*, MemorySpace>("col", 0);
    ko::Profiling::popRegion();
    ko::Profiling::pushRegion("query BVH");
    ArborX::query(bvh, ExecutionSpace{}, Spheres<MemorySpace>{fsubX, frad},
                  spmat_views.col, spmat_views.rowmap);
    ko::Profiling::popRegion();

    nnz = spmat_views.col.size();

    // keeping this around, just in case
    // sort the tree-generated columns, to make life easier when calculating val
    // for (int i = 0; i < lNp; ++i) {
    //   auto sort_view = ko::subview(
    //       spmat_views.col,
    //       ko::make_pair(spmat_views.rowmap(i), spmat_views.rowmap(i + 1)));
    //   auto device_permutation =
    //       ArborX::Details::sortObjects(ExecutionSpace{}, sort_view);
    // }

    // FIXME:
    // keeping this around, since it makes some of the transfer mat calculations
    // easier, though it could probably be eliminated with some cleverness
    spmat_views.row = ko::View<int*>("row", nnz);
    ko::parallel_for(
        "compute_val", lNp, KOKKOS_LAMBDA(const int& i) {
          int begin = spmat_views.rowmap(i);
          int end = spmat_views.rowmap(i + 1);
          for (int j = begin; j < end; ++j) {
            spmat_views.row(j) = i;
          }
        });

    spmat_views.val = ko::View<Real*>("val", nnz);
    ko::parallel_for(
        "compute_val", nnz, KOKKOS_LAMBDA(const int& i) {
          Real dim_sum = 0.0;
          for (int k = 0; k < ldim; ++k) {
            dim_sum +=
                pow(lX(k, spmat_views.row(i)) - lX(k, spmat_views.col(i)), 2);
          }
          spmat_views.val(i) = exp(dim_sum / -denom) / c;
        });
    return spmat_views;
  }
};
}  // namespace particles

#endif
