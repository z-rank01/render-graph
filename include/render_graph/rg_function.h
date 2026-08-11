#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace render_graph
{
    // A lightweight, small-buffer-optimized callable wrapper similar to std::function,
    // designed for render-graph pass callbacks.
    //
    // - Stores small callables inline (no heap) when they are nothrow-movable and fit.
    // - Falls back to heap allocation otherwise (still supports capturing lambdas).
    // - Copyable/movable; empty state supported.

    template <typename Signature, std::size_t InlineBytes = 64>
    class rg_function;

    template <typename R, typename... Args, std::size_t InlineBytes>
    class rg_function<R(Args...), InlineBytes>
    {
    public:
        rg_function() noexcept = default;
        rg_function(std::nullptr_t) noexcept {}

        rg_function(const rg_function& other)
        {
            if (other.vtable != nullptr)
            {
                other.vtable->copy(*this, other);
            }
        }

        rg_function(rg_function&& other) noexcept
        {
            if (other.vtable != nullptr)
            {
                other.vtable->move(*this, other);
            }
        }

        rg_function& operator=(const rg_function& other)
        {
            if (this == &other)
            {
                return *this;
            }
            reset();
            if (other.vtable != nullptr)
            {
                other.vtable->copy(*this, other);
            }
            return *this;
        }

        rg_function& operator=(rg_function&& other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }
            reset();
            if (other.vtable != nullptr)
            {
                other.vtable->move(*this, other);
            }
            return *this;
        }

        ~rg_function() { reset(); }

        template <typename F>
            requires(!std::is_same_v<std::remove_cvref_t<F>, rg_function>)
        rg_function(F&& func)
        {
            emplace(std::forward<F>(func));
        }

        template <typename F>
            requires(!std::is_same_v<std::remove_cvref_t<F>, rg_function>)
        rg_function& operator=(F&& func)
        {
            reset();
            emplace(std::forward<F>(func));
            return *this;
        }

        explicit operator bool() const noexcept { return vtable != nullptr; }

        R operator()(Args... args) const
        {
            // Match std::function: calling empty is UB; keep it simple and fast.
            return vtable->invoke(*this, std::forward<Args>(args)...);
        }

        void reset() noexcept
        {
            if (vtable != nullptr)
            {
                vtable->destroy(*this);
                vtable = nullptr;
                object = nullptr;
                heap = false;
            }
        }

        void swap(rg_function& other) noexcept
        {
            if (this == &other)
            {
                return;
            }

            // Fallback swap via moves/copies; still noexcept because move() is noexcept
            // for our storage strategy (inline only when nothrow-movable).
            rg_function tmp(std::move(other));
            other = std::move(*this);
            *this = std::move(tmp);
        }

    private:
        struct vtable_t
        {
            R (*invoke)(const rg_function&, Args&&...);
            void (*destroy)(rg_function&) noexcept;
            void (*copy)(rg_function& dst, const rg_function& src);
            void (*move)(rg_function& dst, rg_function& src) noexcept;
        };

        template <typename F>
        static constexpr bool k_inline_ok =
            (sizeof(F) <= InlineBytes) &&
            (alignof(F) <= alignof(std::max_align_t)) &&
            std::is_nothrow_move_constructible_v<F>;

        template <typename F>
        static const vtable_t* table() noexcept
        {
            static const vtable_t vt{
                // invoke
                [](const rg_function& self, Args&&... call_args) -> R
                {
                    const F* fn = static_cast<const F*>(self.object);
                    if constexpr (std::is_void_v<R>)
                    {
                        (*fn)(std::forward<Args>(call_args)...);
                    }
                    else
                    {
                        return (*fn)(std::forward<Args>(call_args)...);
                    }
                },
                // destroy
                [](rg_function& self) noexcept
                {
                    if (self.object == nullptr)
                    {
                        return;
                    }
                    if (self.heap)
                    {
                        delete static_cast<F*>(self.object);
                    }
                    else
                    {
                        static_cast<F*>(self.object)->~F();
                    }
                    self.object = nullptr;
                    self.heap = false;
                },
                // copy
                [](rg_function& dst, const rg_function& src)
                {
                    const F* src_fn = static_cast<const F*>(src.object);
                    if constexpr (k_inline_ok<F>)
                    {
                        dst.object = dst.storage();
                        dst.heap = false;
                        ::new (dst.object) F(*src_fn);
                    }
                    else
                    {
                        static_assert(std::is_copy_constructible_v<F>, "rg_function target must be copy-constructible");
                        dst.object = new F(*src_fn);
                        dst.heap = true;
                    }
                    dst.vtable = src.vtable;
                },
                // move
                [](rg_function& dst, rg_function& src) noexcept
                {
                    if (src.object == nullptr)
                    {
                        dst.vtable = nullptr;
                        dst.object = nullptr;
                        dst.heap = false;
                        return;
                    }

                    if (src.heap)
                    {
                        dst.object = src.object;
                        dst.heap = true;
                        dst.vtable = src.vtable;

                        src.object = nullptr;
                        src.heap = false;
                        src.vtable = nullptr;
                        return;
                    }

                    // Inline storage: only enabled for nothrow-movable types.
                    dst.object = dst.storage();
                    dst.heap = false;
                    dst.vtable = src.vtable;
                    ::new (dst.object) F(std::move(*static_cast<F*>(src.object)));
                    static_cast<F*>(src.object)->~F();

                    src.object = nullptr;
                    src.heap = false;
                    src.vtable = nullptr;
                },
            };
            return &vt;
        }

        template <typename F>
        void emplace(F&& func)
        {
            using Fn = std::decay_t<F>;

            if constexpr (k_inline_ok<Fn>)
            {
                object = storage();
                heap = false;
                ::new (object) Fn(std::forward<F>(func));
            }
            else
            {
                static_assert(std::is_copy_constructible_v<Fn> || std::is_move_constructible_v<Fn>,
                              "rg_function target must be copy/move constructible");
                object = new Fn(std::forward<F>(func));
                heap = true;
            }

            vtable = table<Fn>();
        }

        void* storage() noexcept { return static_cast<void*>(&storage_buf); }
        const void* storage() const noexcept { return static_cast<const void*>(&storage_buf); }

        typename std::aligned_storage<InlineBytes, alignof(std::max_align_t)>::type storage_buf{};
        const vtable_t* vtable = nullptr;
        void* object = nullptr;
        bool heap = false;
    };

} // namespace render_graph
