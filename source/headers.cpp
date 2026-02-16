#include "http/headers.h"

namespace http
{
  headers::headers(std::vector<header> data)
    : data_(std::move(data))
  {
  }

  void headers::add(std::string name, std::string value)
  {
    data_.push_back({std::move(name), std::move(value)});
  }

  size_t headers::size() const
  {
    return data_.size();
  }

  const header& headers::operator[](size_t index) const
  {
    return data_[index];
  }

  const std::vector<header>& headers::get() const
  {
    return data_;
  }

  std::string headers::get(std::string_view name) const
  {
    for (const auto& h : data_)
    {
      if (h.name == name)
        return h.value;
    }
    return "";
  }
}
