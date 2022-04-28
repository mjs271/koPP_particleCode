#ifndef RANDOM_HPP
#define RANDOM_HPP

#include "Kokkos_Core.hpp"
#include "Kokkos_Random.hpp"
#include "type_defs.hpp"

namespace particles {

// RandPoolType rand_pool;

RandPoolType init_random_seed();
RandPoolType init_random_seed(const int& seed);

// functor for generating uniformly-distributed random doubles
// in the range [start, end]
// Note: RandPoolType is currently hard-coded in the particle class
// GeneratorPool type
template <class RandPool>
struct RandomUniform {
  // Output View for the random numbers
  ko::View<Real*> vals;

  // The GeneratorPool
  RandPool rand_pool;

  typedef Real Scalar;
  typedef typename RandPool::generator_type gen_type;

  // mean and variance of the normal random variable
  Scalar start, end;

  KOKKOS_INLINE_FUNCTION
  void operator()(int i) const {
    // Get a random number state from the pool for the active thread
    gen_type rgen = rand_pool.get_state();

    // draw random normal numbers, with mean and variance provided
    vals(i) = rgen.drand(start, end);

    // Give the state back, which will allow another thread to acquire it
    rand_pool.free_state(rgen);
  }

  // Constructor, Initialize all members
  RandomUniform(ko::View<Real*> vals_, const RandPool& rand_pool_,
                const Scalar& start_, const Scalar& end_)
      : vals(vals_), rand_pool(rand_pool_), start(start_), end(end_) {}

};  // end RandomUniform functor

}  // namespace particles

#endif