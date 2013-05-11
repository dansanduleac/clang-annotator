#ifndef ANNOTATEVARIABLES_COMMON_H
#define ANNOTATEVARIABLES_COMMON_H

#include "Flags.h"
#include "StringJoin.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include "clang/AST/AST.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include <new>
#include <sstream>

namespace assertions {

using namespace clang;

struct Color {
  raw_ostream::Colors color;
  bool bold;

  Color(raw_ostream::Colors col, bool bol = true) : color(col), bold(bol) {}
};

struct Normal {} normal;

raw_ostream& operator<<(raw_ostream& OS, const Color& c) {
  return OS.changeColor(c.color, c.bold);
}

raw_ostream& operator<<(raw_ostream& OS, const Normal&) {
  return OS.resetColor();
}

static const Color green = raw_ostream::GREEN;
static const Color blue = raw_ostream::BLUE;
static const Color red = raw_ostream::RED;
static const Color magenta = raw_ostream::MAGENTA;
static const Color yellow = raw_ostream::YELLOW;
static const Color savedColor = raw_ostream::SAVEDCOLOR;


const llvm::StringRef GLOBAL_PREFIX = "assertion,";

class Common {
  ASTContext* Context;
  Rewriter* Rewriter;

public:
  Common(ASTContext* C, class Rewriter* Rewriter)
    : Context(C), Rewriter(Rewriter) { }

  ASTContext* getContext() { return Context; }

  // Convenience functions for debugging and diagnostics.
  // ----------------------------------------------------

  template <typename T>
  std::string printLocInternal(T* D) {
    FullSourceLoc FullLocation = Context->getFullLoc(D->getLocStart());
    if (FullLocation.isValid()) {
      std::ostringstream ss;
      ss << FullLocation.getSpellingLineNumber() << ":"
         << FullLocation.getSpellingColumnNumber();
      return ss.str();
    } else {
      return "[INVALID LOCATION]";
    }
  }

  std::string printLoc(Stmt* S) {
    return printLocInternal(S);
  }

  std::string printLoc(Decl* D) {
    return printLocInternal(D);
  }

  template <typename T>
  DiagnosticBuilder warnAt(T* X, StringRef s) {
    return diagnosticAt(X, s, DiagnosticsEngine::Warning);
  }


  template <typename T>
  static SourceRange getSourceRange(T&& X, const llvm::false_type&) {
    return X.getSourceRange();
  }

  static SourceRange getSourceRange(Attr&& X, const llvm::true_type&) {
    return X.getRange();
  }

  // Get the implicit SourceRange for the token pointed to by this
  // SourceLocation.
  static SourceRange getSourceRange(SourceLocation L) {
    return L;
  }

  template <bool Cond>
  struct as_truth : llvm::conditional<Cond, llvm::true_type, llvm::false_type> {};

  template <typename T>
  static SourceRange getSourceRange(T* X) {
    typename as_truth<llvm::is_base_of<Attr,T>::value>::type choice;
    return getSourceRange(std::move(*X), choice);
  }

  // For exact types being sent.
  template <typename T>
  static SourceRange getSourceRange(T&& X) {
    typename as_truth<llvm::is_base_of<Attr, llvm::remove_reference<T>>::value>::type choice;
    return getSourceRange(X, choice);
  }

  // For convenience, to highlight an AST node (of type T).
  template <typename T>
  DiagnosticBuilder
  diagnosticAt(T&& X, StringRef str,
               DiagnosticsEngine::Level lvl = DiagnosticsEngine::Error) {
    // All AST node ranges are token ranges from what I know.
    // TODO apart from the SourceLocation needed, we can just << X
    // everything... (making the below function obsolete-ish..)
    return diagnosticAt(CharSourceRange::getTokenRange(getSourceRange(X)),
                        str, lvl);
  }

  DiagnosticBuilder
  diagnosticAt(CharSourceRange csr, StringRef str,
               DiagnosticsEngine::Level lvl) {
    DiagnosticsEngine &D = Context->getDiagnostics();
    unsigned DiagID = D.getCustomDiagID(lvl, str);
    // << does .AddSourceRange() too!
    return D.Report(csr.getBegin(), DiagID) << csr;
  }


  // -------------------------------------------------
  // THE ATTRIBUTE
  // -------------------------------------------------
  // TODO:
  // Update these to actually use a custom attribute, not just AnnotateAttr,
  // because this is quite inflexible. That would require modifying the
  // accepted input language, though.

  typedef AnnotateAttr AssertionAttr;

  // Map holding the function parameter assertions for pointer params. We
  // don't want to allow them to remain attached to the ParmVarDecls because
  // then they get codegen'd, and we only want to codegen annotations where
  // we need to initialize state.
  llvm::DenseMap<ParmVarDecl *, AssertionAttr *> ParmAttrMap;

  // There will be a different Visitor for each translation unit, but that's
  // ok because we do not need globally unique UIDs. Each translation unit
  // will have its own set of UIDs, and their mappings to the run-time
  // structures holding the asserted variables' states are maintained
  // internally per TU.
  int uid = 0;

  /// Extract the valid AssertionAttr, if any.
  AssertionAttr *getAssertionAttr(Decl *D) {
    AssertionAttr* attr = D->template getAttr<AssertionAttr>();
    if (!attr && isa<ParmVarDecl>(D)) {
      // Retrieve it from the ParmAttrMap.
      attr = ParmAttrMap.lookup(cast<ParmVarDecl>(D));
    }
    return attr && IsSaneAssertionAttr(attr) ? attr : nullptr;
  }

  template <typename T>
  bool hasAssertionAttr(Decl *D) {
    return getAssertionAttr(D) != nullptr;
  }

  // To check if this Attr is in a sane state, representing a correct AssertionAttr.
  // Using it here since we're emulating on top of AnnotateAttr, and not annotations
  // are valid in our context.
  // TODO: deprecate after we implement a separate Attr.
  static bool IsSaneAssertionAttr(const AssertionAttr *attr) {
    return attr->getAnnotation().startswith(GLOBAL_PREFIX);
  }

  // Returns a string that represents the qualified AnnotateAttr's annotation.
  std::string GetQualifiedAttrString(AnnotateAttr* attr) {
    if (!IsSaneAssertionAttr(attr)) {
      llvm_unreachable("Tried to qualify bad AssertionAttr");
    }
    // TODO also pass an ArrayRef to specify additional "parameters"
    SmallString<30> an = attr->getAnnotation();
    an.append(",");
    llvm::raw_svector_ostream S(an);
    S << ++uid;
    return S.str();
  }

  /// Qualifies an existing attr with an unique ID like QualifyAttr, but
  /// replaces the original attr in memory.
  /// @return the UID for convenience, no point returning same address.
  int QualifyAttrReplace(AssertionAttr* attr) {
    (void) ::new(attr) AssertionAttr(attr->getRange(), *Context,
                                     GetQualifiedAttrString(attr));
    return uid;
  }

  // Return a new AssertionAttr representing the qualified attr.
  // Does NOT check whether attr already qualified, or not sane.
  AssertionAttr* QualifyAttr(AssertionAttr* attr) {
    return new(*Context) AnnotateAttr(attr->getRange(), *Context,
                                      GetQualifiedAttrString(attr));
  }

  static bool IsSameAssertion(AssertionAttr* a1, AssertionAttr* a2) {
    if (a1 == nullptr || a2 == nullptr) {
      return a1 == nullptr && a2 == nullptr;
    }
    SmallVector<StringRef, 3> Arr1, Arr2;
    // Only compare the first 2 elements of each.
    // TODO modify this for assertions which can take parameters
    a1->getAnnotation().split(Arr1, ",", 2); Arr1.resize(2);
    a2->getAnnotation().split(Arr2, ",", 2); Arr2.resize(2);
    return Arr1 == Arr2;
  }

  static StringRef AssertionKindAsString(AssertionAttr* attr) {
    SmallVector<StringRef, 3> Arr;
    attr->getAnnotation().split(Arr, ",", 2);
    assert(Arr[0] == "assertion" && "Not an assertion attribute");
    return Arr[1];
  }
};

typedef Common::AssertionAttr AssertionAttr;

}

#endif