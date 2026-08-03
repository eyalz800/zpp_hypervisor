#pragma once
#include "zpp/heap.h"
#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zpp
{
template <typename T>
using vector = std::vector<T, allocator<T>>;

using string =
    std::basic_string<char, std::char_traits<char>, allocator<char>>;

template <typename T>
using deque = std::deque<T, allocator<T>>;

template <typename T>
using list = std::list<T, allocator<T>>;

template <typename T>
using forward_list = std::forward_list<T, allocator<T>>;

template <typename Key, typename Compare = std::less<Key>>
using set = std::set<Key, Compare, allocator<Key>>;

template <typename Key, typename Compare = std::less<Key>>
using multiset = std::multiset<Key, Compare, allocator<Key>>;

template <typename Key, typename Value, typename Compare = std::less<Key>>
using map = std::map<Key,
                     Value,
                     Compare,
                     allocator<std::pair<const Key, Value>>>;

template <typename Key, typename Value, typename Compare = std::less<Key>>
using multimap = std::multimap<Key,
                               Value,
                               Compare,
                               allocator<std::pair<const Key, Value>>>;

template <typename Key,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
using unordered_set =
    std::unordered_set<Key, Hash, KeyEqual, allocator<Key>>;

template <typename Key,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
using unordered_multiset =
    std::unordered_multiset<Key, Hash, KeyEqual, allocator<Key>>;

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
using unordered_map =
    std::unordered_map<Key,
                       Value,
                       Hash,
                       KeyEqual,
                       allocator<std::pair<const Key, Value>>>;

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
using unordered_multimap =
    std::unordered_multimap<Key,
                            Value,
                            Hash,
                            KeyEqual,
                            allocator<std::pair<const Key, Value>>>;

} // namespace zpp
