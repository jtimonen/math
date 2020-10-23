#ifndef STAN_MATH_REV_FUNCTOR_REDUCE_SUM_HPP
#define STAN_MATH_REV_FUNCTOR_REDUCE_SUM_HPP

#include <stan/math/prim/meta.hpp>
#include <stan/math/prim/functor.hpp>
#include <stan/math/rev/fun/sum.hpp>
#include <stan/math/rev/core.hpp>
#include <stan/math/rev/functor/reverse_pass_callback.hpp>

#include <tbb/task_arena.h>
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range.h>

#include <tuple>
#include <memory>
#include <utility>
#include <vector>

#include <tbb/enumerable_thread_specific.h>

namespace stan {
namespace math {
namespace internal {

/**
 * Var specialization of reduce_sum_impl
 *
 * @tparam ReduceFunction Type of reducer function
 * @tparam ReturnType Must be var
 * @tparam Vec Type of sliced argument
 * @tparam Args Types of shared arguments
 */
template <typename ReduceFunction, typename ReturnType, typename Vec,
          typename... Args>
struct reduce_sum_impl<ReduceFunction, require_var_t<ReturnType>, ReturnType,
                       Vec, Args...> {
  struct partial_context {
    std::vector<var> partial_sum_terms_;
    std::vector<std::decay_t<Vec>> chunked_vmapped_;
    // std::vector<Vec> chunked_vmapped_;
    // using args_tuple_t
    //    = std::tuple<decltype(deep_copy_vars(std::declval<Args>()))...>;
    // args_tuple_t args_tuple_;
    using args_tuple_t = std::tuple<Args...>;
    args_tuple_t args_tuple_;

    template <typename... ArgsT>
    explicit partial_context(ArgsT&&... args_tuple)
        : partial_sum_terms_(),
          chunked_vmapped_(),
          // args_tuple_(deep_copy_vars(args_tuple)...) {}
          args_tuple_(args_tuple...) {}
  };

  struct partial_scope {
    ScopedChainableStack stack_;

    std::unique_ptr<partial_context> context_holder_;

    template <typename... ArgsT>
    explicit partial_scope(ArgsT&&... args_tuple)
        : stack_(), context_holder_(stack_.execute([&]() -> partial_context* {
            return new partial_context(std::forward<ArgsT>(args_tuple)...);
          })) {}
  };

  using local_partial_scopes_t = tbb::enumerable_thread_specific<partial_scope>;

  struct child_scopes_vari : public vari_base {
    std::vector<ScopedChainableStack*> child_scopes_;

    explicit child_scopes_vari(std::vector<ScopedChainableStack*> child_scopes)
        : child_scopes_(child_scopes) {
      ChainableStack::instance_->var_stack_.push_back(this);
    }

    inline void chain() final {
      for (auto child : child_scopes_) {
        child->execute([]() { grad(); });
      }
    }
    inline void set_zero_adjoint() final {
      for (auto child : child_scopes_) {
        child->execute([]() { set_zero_all_adjoints(); });
      }
    }
    inline void init_dependent() {}
  };

  /**
   * This struct is used by the TBB to accumulate partial
   *  sums over consecutive ranges of the input. To distribute the workload,
   *  the TBB can split larger partial sums into smaller ones in which
   *  case the splitting copy constructor is used. It is designed to
   *  meet the Imperative form requirements of `tbb::parallel_reduce`.
   *
   * @note see link [here](https://tinyurl.com/vp7xw2t) for requirements.
   */
  struct recursive_reducer {
    local_partial_scopes_t& partial_scopes_;
    Vec vmapped_;
    std::ostream* msgs_;

    template <typename VecT>
    recursive_reducer(local_partial_scopes_t& partial_scopes, VecT&& vmapped,
                      std::ostream* msgs)
        : partial_scopes_(partial_scopes), vmapped_(vmapped), msgs_(msgs) {}

    /*
     * This is the copy operator as required for tbb::parallel_reduce
     *   Imperative form. This requires sum_ and arg_adjoints_ be reset
     *   to zero since the newly created reducer is used to accumulate
     *   an independent partial sum.
     */
    recursive_reducer(recursive_reducer& other, tbb::split)
        : partial_scopes_(other.partial_scopes_),
          vmapped_(other.vmapped_),
          msgs_(other.msgs_) {}

    /**
     * Compute, using nested autodiff, the value and Jacobian of
     *  `ReduceFunction` called over the range defined by r and accumulate those
     *  in member variable sum_ (for the value) and args_adjoints_ (for the
     *  Jacobian). The nested autodiff uses deep copies of the involved operands
     *  ensuring that no side effects are implied to the adjoints of the input
     *  operands which reside potentially on a autodiff tape stored in a
     *  different thread other than the current thread of execution. This
     * function may be called multiple times per object instantiation (so the
     * sum_ and args_adjoints_ must be accumulated, not just assigned).
     *
     * @param r Range over which to compute reduce_sum
     */
    inline void operator()(const tbb::blocked_range<size_t>& r) {
      if (r.empty()) {
        return;
      }

      // Obtain reference to thread local copy of all shared arguments that do
      // not point
      //   back to main autodiff stack
      partial_scope& local_partial_scope = partial_scopes_.local();
      partial_context& context = *(local_partial_scope.context_holder_);

      local_partial_scope.stack_.execute([&] {
        // Put autodiff copies of sliced argument to local stack
        context.chunked_vmapped_.emplace_back();
        std::decay_t<Vec>& local_sub_slice = context.chunked_vmapped_.back();
        // Vec& local_sub_slice = context.chunked_vmapped_.back();
        local_sub_slice.reserve(r.size());
        for (size_t i = r.begin(); i < r.end(); ++i) {
          // local_sub_slice.emplace_back(deep_copy_vars(vmapped_[i]));
          local_sub_slice.emplace_back(vmapped_[i]);
        }

        // Perform calculation
        context.partial_sum_terms_.emplace_back(apply(
            [&](auto&&... args) {
              return ReduceFunction()(local_sub_slice, r.begin(), r.end() - 1,
                                      msgs_, args...);
            },
            context.args_tuple_));
      });
    }

    /**
     * Join reducers. Accumuluate the value (sum_) and Jacobian (arg_adoints_)
     *   of the other reducer.
     *
     * @param rhs Another partial sum
     */
    inline void join(const recursive_reducer& rhs) {
      /*
    sum_ += rhs.sum_;
    if (args_adjoints_.size() != 0 && rhs.args_adjoints_.size() != 0) {
      args_adjoints_ += rhs.args_adjoints_;
    } else if (args_adjoints_.size() == 0 && rhs.args_adjoints_.size() != 0) {
      args_adjoints_ = rhs.args_adjoints_;
    }
      */
    }
  };

  /**
   * Call an instance of the function `ReduceFunction` on every element
   *   of an input sequence and sum these terms.
   *
   * This specialization is parallelized using tbb and works for reverse
   *   mode autodiff.
   *
   * ReduceFunction must define an operator() with the same signature as:
   *   var f(Vec&& vmapped_subset, int start, int end, std::ostream* msgs,
   * Args&&... args)
   *
   * `ReduceFunction` must be default constructible without any arguments
   *
   * Each call to `ReduceFunction` is responsible for computing the
   *   start through end (inclusive) terms of the overall sum. All args are
   * passed from this function through to the `ReduceFunction` instances.
   *   However, only the start through end (inclusive) elements of the vmapped
   * argument are passed to the `ReduceFunction` instances (as the
   * `vmapped_subset` argument).
   *
   * This function distributes computation of the desired sum and the Jacobian
   * of that sum over multiple threads by coordinating calls to `ReduceFunction`
   * instances. Results are stored as precomputed varis in the autodiff tree.
   *
   * If auto partitioning is true, break work into pieces automatically,
   *  taking grainsize as a recommended work size. The partitioning is
   *  not deterministic nor is the order guaranteed in which partial
   *  sums are accumulated. Due to floating point imprecisions this will likely
   *  lead to slight differences in the accumulated results between
   *  multiple runs. If false, break work deterministically into pieces smaller
   *  than or equal to grainsize and accumulate all the partial sums
   *  in the same order. This still may not achieve bitwise reproducibility.
   *
   * @param vmapped Vector containing one element per term of sum
   * @param auto_partitioning Work partitioning style
   * @param grainsize Suggested grainsize for tbb
   * @param[in, out] msgs The print stream for warning messages
   * @param args Shared arguments used in every sum term
   * @return Summation of all terms
   */
  inline var operator()(Vec&& vmapped, bool auto_partitioning, int grainsize,
                        std::ostream* msgs, Args&&... args) const {
    const std::size_t num_terms = vmapped.size();

    if (vmapped.empty()) {
      return var(0.0);
    }

    const std::size_t num_vars_per_term = count_vars(vmapped[0]);
    const std::size_t num_vars_sliced_terms = num_terms * num_vars_per_term;
    const std::size_t num_vars_shared_terms = count_vars(args...);

    /*
    vari** varis = ChainableStack::instance_->memalloc_.alloc_array<vari*>(
        num_vars_sliced_terms + num_vars_shared_terms);
    double* partials = ChainableStack::instance_->memalloc_.alloc_array<double>(
        num_vars_sliced_terms + num_vars_shared_terms);

    save_varis(varis, vmapped);
    save_varis(varis + num_vars_sliced_terms, args...);

    for (size_t i = 0; i < num_vars_sliced_terms; ++i) {
      partials[i] = 0.0;
    }
    */

    std::shared_ptr<local_partial_scopes_t> local_partial_scopes(
        new local_partial_scopes_t([&] { return partial_scope(args...); }));
    // @Steve: This should work, but fails to compile???
    // local_args_tuple_t local_args(args...);

    recursive_reducer worker(*local_partial_scopes, std::forward<Vec>(vmapped),
                             msgs);

    if (auto_partitioning) {
      tbb::parallel_reduce(
          tbb::blocked_range<std::size_t>(0, num_terms, grainsize), worker);
    } else {
      tbb::simple_partitioner partitioner;
      tbb::parallel_deterministic_reduce(
          tbb::blocked_range<std::size_t>(0, num_terms, grainsize), worker,
          partitioner);
    }

    // reverse pass trick... abused to keep a copy of the shared
    // pointer here
    reverse_pass_callback([local_partial_scopes]() mutable {});

    std::vector<ScopedChainableStack*> child_scope_ref;

    local_partial_scopes->combine_each([&](partial_scope& scope) {
      child_scope_ref.emplace_back(&scope.stack_);
    });

    new child_scopes_vari(child_scope_ref);

    std::vector<var> partials;

    local_partial_scopes->combine_each([&](const partial_scope& scope) {
      partials.emplace_back(
          stan::math::sum(scope.context_holder_->partial_sum_terms_));
    });

    var sum = stan::math::sum(partials);

    return sum;
  }
};
}  // namespace internal

}  // namespace math
}  // namespace stan

#endif
