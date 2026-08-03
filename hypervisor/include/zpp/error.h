#pragma once
#include <string_view>
#include <type_traits>
#include <utility>

namespace zpp
{
template <typename ErrorCode>
decltype(auto) category()
{
    return category(ErrorCode{});
}

class error_category
{
public:
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view message(int code) const noexcept = 0;

    constexpr bool success(int code) const
    {
        return code == m_success_code;
    }

protected:
    constexpr error_category(int success_code) :
        m_success_code(success_code)
    {
    }

    ~error_category() = default;

private:
    int m_success_code{};
};

template <typename ErrorCode, typename Messages>
constexpr auto make_error_category(std::string_view name,
                                   ErrorCode success_code,
                                   Messages && messages)
{
    class category : public error_category,
                     private std::remove_reference_t<Messages>
    {
    public:
        constexpr category(std::string_view name,
                           ErrorCode success_code,
                           Messages && messages) :
            error_category(
                std::underlying_type_t<ErrorCode>(success_code)),
            std::remove_reference_t<Messages>(
                std::forward<Messages>(messages)),
            m_name(name)
        {
        }

        std::string_view name() const noexcept override
        {
            return m_name;
        }

        std::string_view message(int code) const noexcept override
        {
            return this->operator()(ErrorCode{code});
        }

    private:
        std::string_view m_name;
    } category(name, success_code, std::forward<Messages>(messages));

    return category;
}

class error
{
public:
    error() = delete;

    template <typename ErrorCode>
    constexpr error(ErrorCode error_code) :
        m_category(std::addressof(zpp::category<ErrorCode>())),
        m_code(std::underlying_type_t<ErrorCode>(error_code))
    {
    }

    template <typename ErrorCode>
    constexpr error(ErrorCode error_code, const error_category & category) :
        m_category(std::addressof(category)),
        m_code(std::underlying_type_t<ErrorCode>(error_code))
    {
    }

    constexpr const error_category & category() const
    {
        return *m_category;
    }

    constexpr int code() const
    {
        return m_code;
    }

    constexpr std::string_view message() const
    {
        return m_category->message(m_code);
    }

    constexpr explicit operator bool() const
    {
        return m_category->success(m_code);
    }

    static constexpr std::string_view no_error{};

private:
    const error_category * m_category{};
    int m_code{};
};

} // namespace zpp
