//==-- llvm/ADT/ilist.h - Intrusive Linked List Template ---------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines classes to implement an intrusive doubly linked list class
// (i.e. each node of the list must contain a next and previous field for the
// list.
//
// The ilist class itself should be a plug in replacement for list.  This list
// replacement does not provide a constant time size() method, so be careful to
// use empty() when you really want to know if it's empty.
//
// The ilist class is implemented by allocating a 'tail' node when the list is
// created (using ilist_traits<>::createSentinel()).  This tail node is
// absolutely required because the user must be able to compute end()-1. Because
// of this, users of the direct next/prev links will see an extra link on the
// end of the list, which should be ignored.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_ILIST_H
#define LLVM_ADT_ILIST_H

#include "llvm/Support/Compiler.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <type_traits>

namespace llvm {

template<typename NodeTy, typename Traits> class iplist;
template<typename NodeTy> class ilist_iterator;

/// An access class for next/prev on ilist_nodes.
///
/// This gives access to the private parts of ilist nodes.  Nodes for an ilist
/// should friend this class if they inherit privately from ilist_node.
///
/// It's strongly discouraged to *use* this class outside of ilist
/// implementation.
struct ilist_node_access {
  template <typename NodeTy> static NodeTy *getPrev(NodeTy *N) {
    return N->getPrev();
  }
  template <typename NodeTy> static NodeTy *getNext(NodeTy *N) {
    return N->getNext();
  }
  template <typename NodeTy> static const NodeTy *getPrev(const NodeTy *N) {
    return N->getPrev();
  }
  template <typename NodeTy> static const NodeTy *getNext(const NodeTy *N) {
    return N->getNext();
  }

  template <typename NodeTy> static void setPrev(NodeTy *N, NodeTy *Prev) {
    N->setPrev(Prev);
  }
  template <typename NodeTy> static void setNext(NodeTy *N, NodeTy *Next) {
    N->setNext(Next);
  }
  template <typename NodeTy> static void setPrev(NodeTy *N, std::nullptr_t) {
    N->setPrev(nullptr);
  }
  template <typename NodeTy> static void setNext(NodeTy *N, std::nullptr_t) {
    N->setNext(nullptr);
  }
};

namespace ilist_detail {

template <class T> T &make();

/// Type trait to check for a traits class that has a getNext member (as a
/// canary for any of the ilist_nextprev_traits API).
template <class TraitsT, class NodeT> struct HasGetNext {
  typedef char Yes[1];
  typedef char No[2];
  template <size_t N> struct SFINAE {};

  template <class U>
  static Yes &test(U *I, decltype(I->getNext(&make<NodeT>())) * = 0);
  template <class> static No &test(...);

public:
  static const bool value = sizeof(test<TraitsT>(nullptr)) == sizeof(Yes);
};

template <class TraitsT, class NodeT> struct HasObsoleteCustomization {
  static const bool value = HasGetNext<TraitsT, NodeT>::value;
};

} // end namespace ilist_detail

template<typename NodeTy>
struct ilist_traits;

/// ilist_sentinel_traits - A fragment for template traits for intrusive list
/// that provides default sentinel implementations for common operations.
///
/// ilist_sentinel_traits implements a lazy dynamic sentinel allocation
/// strategy. The sentinel is stored in the prev field of ilist's Head.
///
template<typename NodeTy>
struct ilist_sentinel_traits {
  /// createSentinel - create the dynamic sentinel
  static NodeTy *createSentinel() { return new NodeTy(); }

  /// destroySentinel - deallocate the dynamic sentinel
  static void destroySentinel(NodeTy *N) { delete N; }

  /// provideInitialHead - when constructing an ilist, provide a starting
  /// value for its Head
  /// @return null node to indicate that it needs to be allocated later
  static NodeTy *provideInitialHead() { return nullptr; }

  /// ensureHead - make sure that Head is either already
  /// initialized or assigned a fresh sentinel
  /// @return the sentinel
  static NodeTy *ensureHead(NodeTy *&Head) {
    if (!Head) {
      Head = ilist_traits<NodeTy>::createSentinel();
      ilist_traits<NodeTy>::noteHead(Head, Head);
      ilist_node_access::setNext(Head, nullptr);
      return Head;
    }
    return ilist_node_access::getPrev(Head);
  }

  /// noteHead - stash the sentinel into its default location
  static void noteHead(NodeTy *NewHead, NodeTy *Sentinel) {
    ilist_node_access::setPrev(NewHead, Sentinel);
  }
};

template <typename NodeTy> class ilist_half_node;
template <typename NodeTy> class ilist_node;

/// Traits with an embedded ilist_node as a sentinel.
template <typename NodeTy> struct ilist_embedded_sentinel_traits {
  /// Get hold of the node that marks the end of the list.
  ///
  /// FIXME: This downcast is UB.  See llvm.org/PR26753.
  LLVM_NO_SANITIZE("object-size")
  NodeTy *createSentinel() const {
    // Since i(p)lists always publicly derive from their corresponding traits,
    // placing a data member in this class will augment the i(p)list.  But since
    // the NodeTy is expected to be publicly derive from ilist_node<NodeTy>,
    // there is a legal viable downcast from it to NodeTy. We use this trick to
    // superimpose an i(p)list with a "ghostly" NodeTy, which becomes the
    // sentinel. Dereferencing the sentinel is forbidden (save the
    // ilist_node<NodeTy>), so no one will ever notice the superposition.
    return static_cast<NodeTy *>(&Sentinel);
  }
  static void destroySentinel(NodeTy *) {}

  NodeTy *provideInitialHead() const { return createSentinel(); }
  NodeTy *ensureHead(NodeTy *) const { return createSentinel(); }
  static void noteHead(NodeTy *, NodeTy *) {}

private:
  mutable ilist_node<NodeTy> Sentinel;
};

/// Trait with an embedded ilist_half_node as a sentinel.
template <typename NodeTy> struct ilist_half_embedded_sentinel_traits {
  /// Get hold of the node that marks the end of the list.
  ///
  /// FIXME: This downcast is UB.  See llvm.org/PR26753.
  LLVM_NO_SANITIZE("object-size")
  NodeTy *createSentinel() const {
    // See comment in ilist_embedded_sentinel_traits::createSentinel().
    return static_cast<NodeTy *>(&Sentinel);
  }
  static void destroySentinel(NodeTy *) {}

  NodeTy *provideInitialHead() const { return createSentinel(); }
  NodeTy *ensureHead(NodeTy *) const { return createSentinel(); }
  static void noteHead(NodeTy *, NodeTy *) {}

private:
  mutable ilist_half_node<NodeTy> Sentinel;
};

/// Traits with an embedded full node as a sentinel.
template <typename NodeTy> struct ilist_full_embedded_sentinel_traits {
  /// Get hold of the node that marks the end of the list.
  NodeTy *createSentinel() const { return &Sentinel; }
  static void destroySentinel(NodeTy *) {}

  NodeTy *provideInitialHead() const { return createSentinel(); }
  NodeTy *ensureHead(NodeTy *) const { return createSentinel(); }
  static void noteHead(NodeTy *, NodeTy *) {}

private:
  mutable NodeTy Sentinel;
};

/// ilist_node_traits - A fragment for template traits for intrusive list
/// that provides default node related operations.
///
template<typename NodeTy>
struct ilist_node_traits {
  static NodeTy *createNode(const NodeTy &V) { return new NodeTy(V); }
  static void deleteNode(NodeTy *V) { delete V; }

  void addNodeToList(NodeTy *) {}
  void removeNodeFromList(NodeTy *) {}
  void transferNodesFromList(ilist_node_traits &    /*SrcTraits*/,
                             ilist_iterator<NodeTy> /*first*/,
                             ilist_iterator<NodeTy> /*last*/) {}
};

/// ilist_default_traits - Default template traits for intrusive list.
/// By inheriting from this, you can easily use default implementations
/// for all common operations.
///
template <typename NodeTy>
struct ilist_default_traits : public ilist_sentinel_traits<NodeTy>,
                              public ilist_node_traits<NodeTy> {};

// Template traits for intrusive list.  By specializing this template class, you
// can change what next/prev fields are used to store the links...
template<typename NodeTy>
struct ilist_traits : public ilist_default_traits<NodeTy> {};

// Const traits are the same as nonconst traits...
template<typename Ty>
struct ilist_traits<const Ty> : public ilist_traits<Ty> {};

namespace ilist_detail {
template <class NodeTy> struct ConstCorrectNodeType {
  typedef ilist_node<NodeTy> type;
};
template <class NodeTy> struct ConstCorrectNodeType<const NodeTy> {
  typedef const ilist_node<NodeTy> type;
};
} // end namespace ilist_detail

//===----------------------------------------------------------------------===//
// Iterator for intrusive list.
//
template <typename NodeTy>
class ilist_iterator
    : public std::iterator<std::bidirectional_iterator_tag, NodeTy, ptrdiff_t> {
public:
  typedef std::iterator<std::bidirectional_iterator_tag, NodeTy, ptrdiff_t>
      super;

  typedef typename super::value_type value_type;
  typedef typename super::difference_type difference_type;
  typedef typename super::pointer pointer;
  typedef typename super::reference reference;

  typedef typename std::add_const<value_type>::type *const_pointer;
  typedef typename std::add_const<value_type>::type &const_reference;

  typedef typename ilist_detail::ConstCorrectNodeType<NodeTy>::type node_type;
  typedef node_type *node_pointer;
  typedef node_type &node_reference;

private:
  pointer NodePtr;

public:
  /// Create from an ilist_node.
  explicit ilist_iterator(node_reference N)
      : NodePtr(static_cast<NodeTy *>(&N)) {}

  explicit ilist_iterator(pointer NP) : NodePtr(NP) {}
  explicit ilist_iterator(reference NR) : NodePtr(&NR) {}
  ilist_iterator() : NodePtr(nullptr) {}

  // This is templated so that we can allow constructing a const iterator from
  // a nonconst iterator...
  template <class node_ty>
  ilist_iterator(
      const ilist_iterator<node_ty> &RHS,
      typename std::enable_if<std::is_convertible<node_ty *, NodeTy *>::value,
                              void *>::type = nullptr)
      : NodePtr(RHS.getNodePtrUnchecked()) {}

  // This is templated so that we can allow assigning to a const iterator from
  // a nonconst iterator...
  template <class node_ty>
  const ilist_iterator &operator=(const ilist_iterator<node_ty> &RHS) {
    NodePtr = RHS.getNodePtrUnchecked();
    return *this;
  }

  void reset(pointer NP) { NodePtr = NP; }

  // Accessors...
  reference operator*() const { return *NodePtr; }
  pointer operator->() const { return &operator*(); }

  // Comparison operators
  friend bool operator==(const ilist_iterator &LHS, const ilist_iterator &RHS) {
    return LHS.NodePtr == RHS.NodePtr;
  }
  friend bool operator!=(const ilist_iterator &LHS, const ilist_iterator &RHS) {
    return LHS.NodePtr != RHS.NodePtr;
  }

  // Increment and decrement operators...
  ilist_iterator &operator--() {
    NodePtr = ilist_node_access::getPrev(NodePtr);
    assert(NodePtr && "--'d off the beginning of an ilist!");
    return *this;
  }
  ilist_iterator &operator++() {
    NodePtr = ilist_node_access::getNext(NodePtr);
    return *this;
  }
  ilist_iterator operator--(int) {
    ilist_iterator tmp = *this;
    --*this;
    return tmp;
  }
  ilist_iterator operator++(int) {
    ilist_iterator tmp = *this;
    ++*this;
    return tmp;
  }

  /// Get the underlying ilist_node.
  node_pointer getNodePtr() const { return static_cast<node_pointer>(NodePtr); }

  // Internal interface, do not use...
  pointer getNodePtrUnchecked() const { return NodePtr; }
};

// Allow ilist_iterators to convert into pointers to a node automatically when
// used by the dyn_cast, cast, isa mechanisms...

template<typename From> struct simplify_type;

template<typename NodeTy> struct simplify_type<ilist_iterator<NodeTy> > {
  typedef NodeTy* SimpleType;

  static SimpleType getSimplifiedValue(ilist_iterator<NodeTy> &Node) {
    return &*Node;
  }
};
template<typename NodeTy> struct simplify_type<const ilist_iterator<NodeTy> > {
  typedef /*const*/ NodeTy* SimpleType;

  static SimpleType getSimplifiedValue(const ilist_iterator<NodeTy> &Node) {
    return &*Node;
  }
};


//===----------------------------------------------------------------------===//
//
/// iplist - The subset of list functionality that can safely be used on nodes
/// of polymorphic types, i.e. a heterogeneous list with a common base class that
/// holds the next/prev pointers.  The only state of the list itself is a single
/// pointer to the head of the list.
///
/// This list can be in one of three interesting states:
/// 1. The list may be completely unconstructed.  In this case, the head
///    pointer is null.  When in this form, any query for an iterator (e.g.
///    begin() or end()) causes the list to transparently change to state #2.
/// 2. The list may be empty, but contain a sentinel for the end iterator. This
///    sentinel is created by the Traits::createSentinel method and is a link
///    in the list.  When the list is empty, the pointer in the iplist points
///    to the sentinel.  Once the sentinel is constructed, it
///    is not destroyed until the list is.
/// 3. The list may contain actual objects in it, which are stored as a doubly
///    linked list of nodes.  One invariant of the list is that the predecessor
///    of the first node in the list always points to the last node in the list,
///    and the successor pointer for the sentinel (which always stays at the
///    end of the list) is always null.
///
template <typename NodeTy, typename Traits = ilist_traits<NodeTy>>
class iplist : public Traits, ilist_node_access {
  // TODO: Drop this assertion and the transitive type traits anytime after
  // v4.0 is branched (i.e,. keep them for one release to help out-of-tree code
  // update).
  static_assert(!ilist_detail::HasObsoleteCustomization<Traits, NodeTy>::value,
                "ilist customization points have changed!");

  mutable NodeTy *Head;

  // Use the prev node pointer of 'head' as the tail pointer.  This is really a
  // circularly linked list where we snip the 'next' link from the sentinel node
  // back to the first node in the list (to preserve assertions about going off
  // the end of the list).
  NodeTy *getTail() { return this->ensureHead(Head); }
  const NodeTy *getTail() const { return this->ensureHead(Head); }
  void setTail(NodeTy *N) const { this->noteHead(Head, N); }

  /// CreateLazySentinel - This method verifies whether the sentinel for the
  /// list has been created and lazily makes it if not.
  void CreateLazySentinel() const {
    this->ensureHead(Head);
  }

  static bool op_less(NodeTy &L, NodeTy &R) { return L < R; }
  static bool op_equal(NodeTy &L, NodeTy &R) { return L == R; }

  // No fundamental reason why iplist can't be copyable, but the default
  // copy/copy-assign won't do.
  iplist(const iplist &) = delete;
  void operator=(const iplist &) = delete;

public:
  typedef NodeTy *pointer;
  typedef const NodeTy *const_pointer;
  typedef NodeTy &reference;
  typedef const NodeTy &const_reference;
  typedef NodeTy value_type;
  typedef ilist_iterator<NodeTy> iterator;
  typedef ilist_iterator<const NodeTy> const_iterator;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef std::reverse_iterator<const_iterator>  const_reverse_iterator;
  typedef std::reverse_iterator<iterator>  reverse_iterator;

  iplist() : Head(this->provideInitialHead()) {}
  ~iplist() {
    if (!Head) return;
    clear();
    Traits::destroySentinel(getTail());
  }

  // Iterator creation methods.
  iterator begin() {
    CreateLazySentinel();
    return iterator(Head);
  }
  const_iterator begin() const {
    CreateLazySentinel();
    return const_iterator(Head);
  }
  iterator end() {
    CreateLazySentinel();
    return iterator(getTail());
  }
  const_iterator end() const {
    CreateLazySentinel();
    return const_iterator(getTail());
  }

  // reverse iterator creation methods.
  reverse_iterator rbegin()            { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const{ return const_reverse_iterator(end()); }
  reverse_iterator rend()              { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const { return const_reverse_iterator(begin());}


  // Miscellaneous inspection routines.
  size_type max_size() const { return size_type(-1); }
  bool LLVM_ATTRIBUTE_UNUSED_RESULT empty() const {
    return !Head || Head == getTail();
  }

  // Front and back accessor functions...
  reference front() {
    assert(!empty() && "Called front() on empty list!");
    return *Head;
  }
  const_reference front() const {
    assert(!empty() && "Called front() on empty list!");
    return *Head;
  }
  reference back() {
    assert(!empty() && "Called back() on empty list!");
    return *this->getPrev(getTail());
  }
  const_reference back() const {
    assert(!empty() && "Called back() on empty list!");
    return *this->getPrev(getTail());
  }

  void swap(iplist &RHS) {
    assert(0 && "Swap does not use list traits callback correctly yet!");
    std::swap(Head, RHS.Head);
  }

  iterator insert(iterator where, NodeTy *New) {
    NodeTy *CurNode = where.getNodePtrUnchecked();
    NodeTy *PrevNode = this->getPrev(CurNode);
    this->setNext(New, CurNode);
    this->setPrev(New, PrevNode);

    if (CurNode != Head)  // Is PrevNode off the beginning of the list?
      this->setNext(PrevNode, New);
    else
      Head = New;
    this->setPrev(CurNode, New);

    this->addNodeToList(New);  // Notify traits that we added a node...
    return iterator(New);
  }

  iterator insert(iterator where, const NodeTy &New) {
    return this->insert(where, new NodeTy(New));
  }

  iterator insertAfter(iterator where, NodeTy *New) {
    if (empty())
      return insert(begin(), New);
    else
      return insert(++where, New);
  }

  NodeTy *remove(iterator &IT) {
    assert(IT != end() && "Cannot remove end of list!");
    NodeTy *Node = &*IT;
    NodeTy *NextNode = this->getNext(Node);
    NodeTy *PrevNode = this->getPrev(Node);

    if (Node != Head)  // Is PrevNode off the beginning of the list?
      this->setNext(PrevNode, NextNode);
    else
      Head = NextNode;
    this->setPrev(NextNode, PrevNode);
    IT.reset(NextNode);
    this->removeNodeFromList(Node);  // Notify traits that we removed a node...

    // Set the next/prev pointers of the current node to null.  This isn't
    // strictly required, but this catches errors where a node is removed from
    // an ilist (and potentially deleted) with iterators still pointing at it.
    // When those iterators are incremented or decremented, they will assert on
    // the null next/prev pointer instead of "usually working".
    this->setNext(Node, nullptr);
    this->setPrev(Node, nullptr);
    return Node;
  }

  NodeTy *remove(const iterator &IT) {
    iterator MutIt = IT;
    return remove(MutIt);
  }

  NodeTy *remove(NodeTy *IT) { return remove(iterator(IT)); }
  NodeTy *remove(NodeTy &IT) { return remove(iterator(IT)); }

  // erase - remove a node from the controlled sequence... and delete it.
  iterator erase(iterator where) {
    this->deleteNode(remove(where));
    return where;
  }

  iterator erase(NodeTy *IT) { return erase(iterator(IT)); }
  iterator erase(NodeTy &IT) { return erase(iterator(IT)); }

  /// Remove all nodes from the list like clear(), but do not call
  /// removeNodeFromList() or deleteNode().
  ///
  /// This should only be used immediately before freeing nodes in bulk to
  /// avoid traversing the list and bringing all the nodes into cache.
  void clearAndLeakNodesUnsafely() {
    if (Head) {
      Head = getTail();
      this->setPrev(Head, Head);
    }
  }

private:
  // transfer - The heart of the splice function.  Move linked list nodes from
  // [first, last) into position.
  //
  void transfer(iterator position, iplist &L2, iterator first, iterator last) {
    assert(first != last && "Should be checked by callers");
    // Position cannot be contained in the range to be transferred.
    // Check for the most common mistake.
    assert(position != first &&
           "Insertion point can't be one of the transferred nodes");

    if (position != last) {
      // Note: we have to be careful about the case when we move the first node
      // in the list.  This node is the list sentinel node and we can't move it.
      NodeTy *ThisSentinel = getTail();
      setTail(nullptr);
      NodeTy *L2Sentinel = L2.getTail();
      L2.setTail(nullptr);

      // Remove [first, last) from its old position.
      NodeTy *First = &*first, *Prev = this->getPrev(First);
      NodeTy *Next = last.getNodePtrUnchecked(), *Last = this->getPrev(Next);
      if (Prev)
        this->setNext(Prev, Next);
      else
        L2.Head = Next;
      this->setPrev(Next, Prev);

      // Splice [first, last) into its new position.
      NodeTy *PosNext = position.getNodePtrUnchecked();
      NodeTy *PosPrev = this->getPrev(PosNext);

      // Fix head of list...
      if (PosPrev)
        this->setNext(PosPrev, First);
      else
        Head = First;
      this->setPrev(First, PosPrev);

      // Fix end of list...
      this->setNext(Last, PosNext);
      this->setPrev(PosNext, Last);

      this->transferNodesFromList(L2, iterator(First), iterator(PosNext));

      // Now that everything is set, restore the pointers to the list sentinels.
      L2.setTail(L2Sentinel);
      setTail(ThisSentinel);
    }
  }

public:

  //===----------------------------------------------------------------------===
  // Functionality derived from other functions defined above...
  //

  size_type LLVM_ATTRIBUTE_UNUSED_RESULT size() const {
    if (!Head) return 0; // Don't require construction of sentinel if empty.
    return std::distance(begin(), end());
  }

  iterator erase(iterator first, iterator last) {
    while (first != last)
      first = erase(first);
    return last;
  }

  void clear() { if (Head) erase(begin(), end()); }

  // Front and back inserters...
  void push_front(NodeTy *val) { insert(begin(), val); }
  void push_back(NodeTy *val) { insert(end(), val); }
  void pop_front() {
    assert(!empty() && "pop_front() on empty list!");
    erase(begin());
  }
  void pop_back() {
    assert(!empty() && "pop_back() on empty list!");
    iterator t = end(); erase(--t);
  }

  // Special forms of insert...
  template<class InIt> void insert(iterator where, InIt first, InIt last) {
    for (; first != last; ++first) insert(where, *first);
  }

  // Splice members - defined in terms of transfer...
  void splice(iterator where, iplist &L2) {
    if (!L2.empty())
      transfer(where, L2, L2.begin(), L2.end());
  }
  void splice(iterator where, iplist &L2, iterator first) {
    iterator last = first; ++last;
    if (where == first || where == last) return; // No change
    transfer(where, L2, first, last);
  }
  void splice(iterator where, iplist &L2, iterator first, iterator last) {
    if (first != last) transfer(where, L2, first, last);
  }
  void splice(iterator where, iplist &L2, NodeTy &N) {
    splice(where, L2, iterator(N));
  }
  void splice(iterator where, iplist &L2, NodeTy *N) {
    splice(where, L2, iterator(N));
  }

  template <class Compare>
  void merge(iplist &Right, Compare comp) {
    if (this == &Right)
      return;
    iterator First1 = begin(), Last1 = end();
    iterator First2 = Right.begin(), Last2 = Right.end();
    while (First1 != Last1 && First2 != Last2) {
      if (comp(*First2, *First1)) {
        iterator Next = First2;
        transfer(First1, Right, First2, ++Next);
        First2 = Next;
      } else {
        ++First1;
      }
    }
    if (First2 != Last2)
      transfer(Last1, Right, First2, Last2);
  }
  void merge(iplist &Right) { return merge(Right, op_less); }

  template <class Compare>
  void sort(Compare comp) {
    // The list is empty, vacuously sorted.
    if (empty())
      return;
    // The list has a single element, vacuously sorted.
    if (std::next(begin()) == end())
      return;
    // Find the split point for the list.
    iterator Center = begin(), End = begin();
    while (End != end() && std::next(End) != end()) {
      Center = std::next(Center);
      End = std::next(std::next(End));
    }
    // Split the list into two.
    iplist RightHalf;
    RightHalf.splice(RightHalf.begin(), *this, Center, end());

    // Sort the two sublists.
    sort(comp);
    RightHalf.sort(comp);

    // Merge the two sublists back together.
    merge(RightHalf, comp);
  }
  void sort() { sort(op_less); }

  /// \brief Get the previous node, or \c nullptr for the list head.
  NodeTy *getPrevNode(NodeTy &N) const {
    auto I = N.getIterator();
    if (I == begin())
      return nullptr;
    return &*std::prev(I);
  }
  /// \brief Get the previous node, or \c nullptr for the list head.
  const NodeTy *getPrevNode(const NodeTy &N) const {
    return getPrevNode(const_cast<NodeTy &>(N));
  }

  /// \brief Get the next node, or \c nullptr for the list tail.
  NodeTy *getNextNode(NodeTy &N) const {
    auto Next = std::next(N.getIterator());
    if (Next == end())
      return nullptr;
    return &*Next;
  }
  /// \brief Get the next node, or \c nullptr for the list tail.
  const NodeTy *getNextNode(const NodeTy &N) const {
    return getNextNode(const_cast<NodeTy &>(N));
  }
};


template<typename NodeTy>
struct ilist : public iplist<NodeTy> {
  typedef typename iplist<NodeTy>::size_type size_type;
  typedef typename iplist<NodeTy>::iterator iterator;

  ilist() {}
  ilist(const ilist &right) : iplist<NodeTy>() {
    insert(this->begin(), right.begin(), right.end());
  }
  explicit ilist(size_type count) {
    insert(this->begin(), count, NodeTy());
  }
  ilist(size_type count, const NodeTy &val) {
    insert(this->begin(), count, val);
  }
  template<class InIt> ilist(InIt first, InIt last) {
    insert(this->begin(), first, last);
  }

  // bring hidden functions into scope
  using iplist<NodeTy>::insert;
  using iplist<NodeTy>::push_front;
  using iplist<NodeTy>::push_back;

  // Main implementation here - Insert for a node passed by value...
  iterator insert(iterator where, const NodeTy &val) {
    return insert(where, this->createNode(val));
  }


  // Front and back inserters...
  void push_front(const NodeTy &val) { insert(this->begin(), val); }
  void push_back(const NodeTy &val) { insert(this->end(), val); }

  void insert(iterator where, size_type count, const NodeTy &val) {
    for (; count != 0; --count) insert(where, val);
  }

  // Assign special forms...
  void assign(size_type count, const NodeTy &val) {
    iterator I = this->begin();
    for (; I != this->end() && count != 0; ++I, --count)
      *I = val;
    if (count != 0)
      insert(this->end(), val, val);
    else
      erase(I, this->end());
  }
  template<class InIt> void assign(InIt first1, InIt last1) {
    iterator first2 = this->begin(), last2 = this->end();
    for ( ; first1 != last1 && first2 != last2; ++first1, ++first2)
      *first1 = *first2;
    if (first2 == last2)
      erase(first1, last1);
    else
      insert(last1, first2, last2);
  }


  // Resize members...
  void resize(size_type newsize, NodeTy val) {
    iterator i = this->begin();
    size_type len = 0;
    for ( ; i != this->end() && len < newsize; ++i, ++len) /* empty*/ ;

    if (len == newsize)
      erase(i, this->end());
    else                                          // i == end()
      insert(this->end(), newsize - len, val);
  }
  void resize(size_type newsize) { resize(newsize, NodeTy()); }
};

} // End llvm namespace

namespace std {
  // Ensure that swap uses the fast list swap...
  template<class Ty>
  void swap(llvm::iplist<Ty> &Left, llvm::iplist<Ty> &Right) {
    Left.swap(Right);
  }
}  // End 'std' extensions...

#endif // LLVM_ADT_ILIST_H
