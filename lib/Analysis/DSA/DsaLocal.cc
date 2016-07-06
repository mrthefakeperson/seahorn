#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetLibraryInfo.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/InstVisitor.h"

#include "llvm/Analysis/MemoryBuiltins.h"

#include "llvm/ADT/DenseSet.h"

#include "seahorn/Analysis/DSA/Graph.hh"
#include "seahorn/Analysis/DSA/Local.hh"
#include "seahorn/Support/SortTopo.hh"

#include "boost/range/algorithm/reverse.hpp"
#include "boost/make_shared.hpp"

#include "avy/AvyDebug.h"

using namespace llvm;
using namespace seahorn;


namespace
{
  std::pair<uint64_t, uint64_t> computeGepOffset (Type *ptrTy, ArrayRef<Value *> Indicies,
                                                  const DataLayout &dl);
  
  template<typename T>
  T gcd(T a, T b)
  {
    T c;
    while(b)
    {
      c = a % b;
      a = b;
      b = c;
    }
    return a;
  }
  
  class BlockBuilderBase
  {
  protected:
    Function &m_func;
    dsa::Graph &m_graph;
    const DataLayout &m_dl;
    const TargetLibraryInfo &m_tli;
    
    
    dsa::Cell valueCell (const Value &v);
    void visitGep (const Value &gep, const Value &base, ArrayRef<Value *> indicies);
    
    bool isSkip (Value &V)
    {
      if (!V.getType ()->isPointerTy ()) return true;
      // XXX skip if only uses are external functions
      return false;
    }
    
  public:
    BlockBuilderBase (Function &func, dsa::Graph &graph,
                      const DataLayout &dl, const TargetLibraryInfo &tli) :
      m_func(func), m_graph(graph), m_dl(dl), m_tli (tli) {}
  };
    
  class InterBlockBuilder : public InstVisitor<InterBlockBuilder>, BlockBuilderBase
  {
    friend class InstVisitor<InterBlockBuilder>;
    
    void visitPHINode (PHINode &PHI);
  public:
    InterBlockBuilder (Function &func, dsa::Graph &graph,
                       const DataLayout &dl, const TargetLibraryInfo &tli) :
      BlockBuilderBase (func, graph, dl, tli) {}
  };
  
  void InterBlockBuilder::visitPHINode (PHINode &PHI)
  {
    if (!PHI.getType ()->isPointerTy ()) return;

    assert (m_graph.hasCell (PHI));
    dsa::Cell &phi = m_graph.mkCell (PHI, dsa::Cell ());
    for (unsigned i = 0, e = PHI.getNumIncomingValues (); i < e; ++i)
    {
      Value &v = *PHI.getIncomingValue (i);
      // -- skip null
      if (isa<Constant> (&v) && cast<Constant> (&v)->isNullValue ()) continue;
      
      dsa::Cell c = valueCell (v);
      assert (!c.isNull ());
      phi.unify (c);
    }
    assert (!phi.isNull ());
  }
  
  class IntraBlockBuilder : public InstVisitor<IntraBlockBuilder>, BlockBuilderBase
  {
    friend class InstVisitor<IntraBlockBuilder>;

    
    void visitAllocaInst (AllocaInst &AI);
    void visitSelectInst(SelectInst &SI);
    void visitLoadInst(LoadInst &LI);
    void visitStoreInst(StoreInst &SI);
    // void visitAtomicCmpXchgInst(AtomicCmpXchgInst &I);
    // void visitAtomicRMWInst(AtomicRMWInst &I);
    void visitReturnInst(ReturnInst &RI);
    // void visitVAArgInst(VAArgInst   &I);
    void visitIntToPtrInst(IntToPtrInst &I);
    void visitPtrToIntInst(PtrToIntInst &I);
    void visitBitCastInst(BitCastInst &I);
    void visitCmpInst(CmpInst &I) {/* do nothing */}
    void visitInsertValueInst(InsertValueInst& I); 
    void visitExtractValueInst(ExtractValueInst& I); 

    void visitGetElementPtrInst(GetElementPtrInst &I);
    void visitInstruction(Instruction &I);

    void visitMemSetInst(MemSetInst &I);            
    void visitMemTransferInst(MemTransferInst &I)  ;
    
    void visitCallSite(CallSite CS);
    // void visitVAStart(CallSite CS);

  public:
     IntraBlockBuilder (Function &func, dsa::Graph &graph,
                       const DataLayout &dl, const TargetLibraryInfo &tli) :
       BlockBuilderBase (func, graph, dl, tli) {}
  };

  
  dsa::Cell BlockBuilderBase::valueCell (const Value &v)
  {
    using namespace dsa;
    assert (v.getType ()->isPointerTy () || v.getType ()->isAggregateType ());
  
    if (isa<Constant> (&v) && cast<Constant> (&v)->isNullValue ())
    {
      LOG ("dsa",
           errs () << "WARNING: not handled constant: " << v << "\n";);
      return Cell();
    }
  
    if (m_graph.hasCell (v))
    {
      Cell &c = m_graph.mkCell (v, Cell ());
      assert (!c.isNull ());
      return c;
    }

    if (isa<UndefValue> (&v)) return Cell();
    if (isa<GlobalAlias> (&v)) return valueCell (*cast<GlobalAlias> (&v)->getAliasee ());

    if (isa<ConstantStruct> (&v) || isa<ConstantArray> (&v) ||
        isa<ConstantDataSequential> (&v) || isa<ConstantDataArray> (&v) ||
        isa<ConstantDataVector> (&v))
    {
      // XXX Handle properly
      assert (false);
      return m_graph.mkCell (v, Cell (m_graph.mkNode (), 0));
    }
      
    // -- special case for aggregate types. Cell creation is handled elsewhere
    if (v.getType ()->isAggregateType ()) return Cell ();
  
    if (const ConstantExpr *ce = dyn_cast<const ConstantExpr> (&v))
    {
      if (ce->isCast () && ce->getOperand (0)->getType ()->isPointerTy ())
        return valueCell (*ce->getOperand (0));
      else if (ce->getOpcode () == Instruction::GetElementPtr)
      {
        Value &base = *(ce->getOperand (0));
        SmallVector<Value*, 8> indicies (ce->op_begin () + 1, ce->op_end ());
        visitGep (v, base, indicies);
        assert (m_graph.hasCell (v));
        return m_graph.mkCell (v, Cell());
      }
    }
  
  
    errs () << v << "\n";
    assert(false && "Not handled expression");
    return Cell();
  
  }    
  
  void IntraBlockBuilder::visitInstruction(Instruction &I)
  {
    if (isSkip (I)) return;
    
    m_graph.mkCell (I, dsa::Cell (m_graph.mkNode (), 0));
  }
  
  void IntraBlockBuilder::visitAllocaInst (AllocaInst &AI)
  {
    using namespace seahorn::dsa;
    assert (!m_graph.hasCell (AI));
    Node &n = m_graph.mkNode ();
    // TODO: record allocation site
    // TODO: mark as stack allocated
    Cell &res = m_graph.mkCell (AI, Cell (n, 0));
  }
  void IntraBlockBuilder::visitSelectInst(SelectInst &SI)
  {
    using namespace seahorn::dsa;
    if (isSkip (SI)) return;
    
    assert (!m_graph.hasCell (SI));

    Cell thenC = valueCell  (*SI.getOperand (1));
    Cell elseC = valueCell  (*SI.getOperand (2));
    thenC.unify (elseC);
    
    // -- create result cell
    m_graph.mkCell (SI, Cell (thenC, 0));
  }
  
 
  void IntraBlockBuilder::visitLoadInst(LoadInst &LI)
  {
    using namespace seahorn::dsa;
    
    Cell base = valueCell  (*LI.getPointerOperand ());
    assert (!base.isNull ());
    base.addType (0, LI.getType ());
    // TODO: mark base as read
    
    // update/create the link
    if (!isSkip (LI)) 
    {
      if (!base.hasLink ())
      {
        Node &n = m_graph.mkNode ();
        base.setLink (0, Cell (&n, 0));
      }
      m_graph.mkCell (LI, base.getLink ());
    }
  }
  
  void IntraBlockBuilder::visitStoreInst(StoreInst &SI)
  {
    using namespace seahorn::dsa;
    
    // -- skip store into NULL
    if (Constant *c = dyn_cast<Constant> (SI.getPointerOperand ()))
      if (c->isNullValue ()) return;
    
    Cell base = valueCell  (*SI.getPointerOperand ());
    assert (!base.isNull ());

    // TODO: mark base as modified

    // XXX: potentially it is enough to update size only at this point
    base.growSize (0, SI.getValueOperand ()->getType ());
    base.addType (0, SI.getValueOperand ()->getType ());
    
    if (!isSkip (*SI.getValueOperand ()))
    {
      Cell val = valueCell  (*SI.getValueOperand ());
      if ((isa<Constant> (SI.getValueOperand ()) &&
           cast<Constant> (SI.getValueOperand ())->isNullValue ()))
      {
        // TODO: mark link as possibly pointing to null
      }
      else
      {
        assert (!val.isNull ());
        base.addLink (0, val);
      }
    }
  }

  
  void IntraBlockBuilder::visitBitCastInst(BitCastInst &I)
  {
    if (isSkip (I)) return;
    dsa::Cell arg = valueCell  (*I.getOperand (0));
    assert (!arg.isNull ());
    m_graph.mkCell (I, arg);
  }
  
  /**
     Computes an offset of a gep instruction for a given type and a
     sequence of indicies.

     The first element of the pair is the fixed offset. The second is
     a gcd of the variable offset.
   */
  std::pair<uint64_t, uint64_t> computeGepOffset (Type *ptrTy, ArrayRef<Value *> Indicies,
                                                  const DataLayout &dl)
  {
    unsigned ptrSz = dl.getPointerSizeInBits ();
    Type *Ty = ptrTy;
    assert (Ty->isPointerTy ());
    
    // numeric offset
    uint64_t noffset = 0;

    // divisor
    uint64_t divisor = 0;
    
    generic_gep_type_iterator<Value* const*>
      TI = gep_type_begin (ptrTy, Indicies);
    for (unsigned CurIDX = 0, EndIDX = Indicies.size (); CurIDX != EndIDX;
         ++CurIDX, ++TI)
    {
      if (StructType *STy = dyn_cast<StructType> (*TI))
      {
        unsigned fieldNo = cast<ConstantInt> (Indicies [CurIDX])->getZExtValue ();
        noffset += dl.getStructLayout (STy)->getElementOffset (fieldNo);
        Ty = STy->getElementType (fieldNo);
      }
      else
      {
        Ty = cast<SequentialType> (Ty)->getElementType ();
        uint64_t sz = dl.getTypeStoreSize (Ty);
        if (ConstantInt *ci = dyn_cast<ConstantInt> (Indicies [CurIDX]))
        {
          int64_t arrayIdx = ci->getSExtValue ();
          noffset += (uint64_t)arrayIdx * sz;
        }
        else
          divisor = divisor == 0 ? sz : gcd (divisor, sz);
      }
    }
    
    return std::make_pair (noffset, divisor);
  }    
  
  /// Computes offset into an indexed type
  uint64_t computeIndexedOffset (Type *ty, ArrayRef<unsigned> indecies,
                                 const DataLayout &dl)
  {
    uint64_t offset = 0;
    for (unsigned idx : indecies)
    {
      if (StructType *sty = dyn_cast<StructType> (ty))
      {
        const StructLayout *layout = dl.getStructLayout (sty);
        offset += layout->getElementOffset (idx);
        ty = sty->getElementType (idx);
      }
      else
      {
        ty = cast<SequentialType> (ty)->getElementType ();
        offset += idx * dl.getTypeAllocSize (ty);
      }
    }
    return offset;
  }
  
  void BlockBuilderBase::visitGep (const Value &gep,
                                    const Value &ptr, ArrayRef<Value *> indicies)
  {
    assert (m_graph.hasCell (ptr) || isa<GlobalValue> (&ptr));
    dsa::Cell base = valueCell (ptr);
    assert (!base.isNull ());

    assert (!m_graph.hasCell (gep));
    dsa::Node *baseNode = base.getNode ();
    if (baseNode->isCollapsed ())
    {
      m_graph.mkCell (gep, dsa::Cell (baseNode, 0));
      return;
    }
    
    auto off = computeGepOffset (ptr.getType (), indicies, m_dl);
    if (off.second)
    {
      // create a node representing the array
      dsa::Node &n = m_graph.mkNode ();
      n.setArraySize (off.second);
      // result of the gep points into that array at the gep offset
      // plus the offset of the base
      m_graph.mkCell (gep, dsa::Cell (n, off.first + base.getOffset ()));
      // finally, unify array with the node of the base 
      n.unify (*baseNode);
    }      
    else
      m_graph.mkCell (gep, dsa::Cell (base, off.first));
  }
  
  void IntraBlockBuilder::visitGetElementPtrInst(GetElementPtrInst &I)
  {
    Value &ptr = *I.getPointerOperand ();
    SmallVector<Value*, 8> indicies (I.op_begin () + 1, I.op_end ());
    visitGep (I, ptr, indicies);
  }
  
  void IntraBlockBuilder::visitInsertValueInst(InsertValueInst& I)
  {
    // TODO: set read/mod/alloc flags
    assert (I.getAggregateOperand ()->getType () == I.getType ());
    using namespace dsa;

    // make sure that the aggregate has a cell
    Cell op = valueCell  (*I.getAggregateOperand ());
    if (op.isNull ())
      // -- create a node for the aggregate
      op = m_graph.mkCell (*I.getAggregateOperand (),
                           Cell (m_graph.mkNode (), 0));
    
    Cell &c = m_graph.mkCell (I, op);

    // -- update type record
    Value &v = *I.getInsertedValueOperand ();
    uint64_t offset = computeIndexedOffset (I.getAggregateOperand ()->getType (),
                                            I.getIndices (), m_dl);
    Cell out (op, offset);
    out.growSize (0, v.getType ());
    out.addType (0, v.getType ());
    
    // -- update link 
    if (!isSkip (v))
    {
      Cell vCell = valueCell  (v);
      assert (!vCell.isNull ());
      out.addLink (0, vCell);
    }
  }
  
  void IntraBlockBuilder::visitExtractValueInst(ExtractValueInst& I)
  {
    // TODO: set read/mod/alloc flags
    using namespace dsa;
    Cell op = valueCell  (*I.getAggregateOperand ());
    if (op.isNull ())
      op = m_graph.mkCell (*I.getAggregateOperand (), Cell (m_graph.mkNode (), 0));
    
    uint64_t offset = computeIndexedOffset (I.getAggregateOperand ()->getType (),
                                            I.getIndices (), m_dl);
    Cell in (op, offset);

    // -- update type record
    in.addType (0, I.getType ());
    
    if (!isSkip (I))
    {
      // -- create a new node if there is no link at this offset yet
      if (!in.hasLink ())
        in.setLink (0, Cell (&m_graph.mkNode (), 0));
      // create cell for the read value and point it to where the link points to
      m_graph.mkCell (I, in.getLink ());
    }
  }
  
  void IntraBlockBuilder::visitCallSite (CallSite CS)
  {
    using namespace dsa;
    if (isSkip (*CS.getInstruction ())) return;
    
    if (llvm::isAllocationFn (CS.getInstruction (), &m_tli, true))
    {
      assert (CS.getInstruction ());
      Node &n = m_graph.mkNode ();
      // TODO: record allocation site
      // TODO: mark as heap allocated
      Cell &res = m_graph.mkCell (*CS.getInstruction (), Cell (n, 0));
      return;  
    }

    Instruction *inst = CS.getInstruction ();
    
    if (inst && !isSkip (*inst))
    {
      Cell &c = m_graph.mkCell (*inst, Cell (m_graph.mkNode (), 0));
      // TODO: mark c as external if it comes from a call to an external function
    }

    
    Value *callee = CS.getCalledValue ()->stripPointerCasts ();
    if (InlineAsm *inasm = dyn_cast<InlineAsm> (callee))
    {
      // TODO: handle inline assembly
    }
    
    // TODO: handle variable argument functions
  }
  
  
  void IntraBlockBuilder::visitMemSetInst (MemSetInst &I)
  {
    // TODO:
    // mark node of I.getDest () as modified
    // can also update size using I.getLength ()
  }
  
  void IntraBlockBuilder::visitMemTransferInst (MemTransferInst &I)
  {
    // TODO: mark I.getDest () is modified, and I.getSource () is read
    assert (m_graph.hasCell (*I.getDest ()));
    // unify the two cells because potentially all bytes of source
    // are copied into dest
    dsa::Cell sourceCell = valueCell  (*I.getSource ());
    m_graph.mkCell(*I.getDest (), dsa::Cell ()).unify (sourceCell);
    // TODO: adjust size of I.getLength ()
    // TODO: handle special case when memcpy is used to move non-pointer value only
  }

  void IntraBlockBuilder::visitIntToPtrInst (IntToPtrInst &I)
  {
    // -- only used as a compare. do not needs DSA node
    if (I.hasOneUse () && isa<CmpInst> (*(I.use_begin ()))) return;
    
    dsa::Node &n = m_graph.mkNode ();
    // TODO: mark n appropriately.
    m_graph.mkCell (I, dsa::Cell (n, 0));
  }
  
  void IntraBlockBuilder::visitReturnInst(ReturnInst &RI)
  {
    Value *v = RI.getReturnValue ();
    if (!v || isSkip (*v)) return;
    
    dsa::Cell c = valueCell  (*v);
    if (c.isNull ()) return;

    m_graph.mkRetCell (m_func, c);
  }
  
  void IntraBlockBuilder::visitPtrToIntInst (PtrToIntInst &I)
  {
    if (I.hasOneUse () && isa<CmpInst> (*(I.use_begin ()))) return;

    if (I.hasOneUse ())
    {
      Value *v = dyn_cast<Value> (*(I.use_begin ()));
      DenseSet<Value *> seen;
      while (v && v->hasOneUse () && seen.insert (v).second)
      {
        if (isa<LoadInst> (v) || isa<StoreInst> (v) || isa<CallInst> (v)) break;
        v = dyn_cast<Value> (*(v->use_begin ()));
      }
      if (isa<BranchInst> (v)) return;
    }
    assert (m_graph.hasCell (*I.getOperand (0)));
    dsa::Cell c = valueCell  (*I.getOperand (0));
    if (!c.isNull ())
      /* c->getNode ()->setPtrToInt (true) */;
  }

  
}


namespace seahorn
{
  namespace dsa
  {

    Local::Local () : 
        ModulePass (ID), m_dl (nullptr), m_tli (nullptr) {}

    void Local::getAnalysisUsage (AnalysisUsage &AU) const 
    {
      AU.addRequired<DataLayoutPass> ();
      AU.addRequired<TargetLibraryInfo> ();
      AU.setPreservesAll ();
    }

    bool Local::runOnModule (Module &M)
    {
      m_dl = &getAnalysis<DataLayoutPass>().getDataLayout ();
      m_tli = &getAnalysis<TargetLibraryInfo> ();
      for (Function &F : M) runOnFunction (F);                
        return false;
    }

    bool Local::runOnFunction (Function &F)
    {
      if (F.isDeclaration () || F.empty ()) return false;
      
      LOG("progress", errs () << "DSA: " << F.getName () << "\n";);
      
      Graph_ptr g = boost::make_shared<Graph> (*m_dl);
      
      // create cells and nodes for formal arguments
      for (Argument &a : F.args ())
        g->mkCell (a, Cell (g->mkNode (), 0));
      
      std::vector<const BasicBlock *> bbs;
      RevTopoSort (F, bbs);
      boost::reverse (bbs);
      
      IntraBlockBuilder intraBuilder (F, *g, *m_dl, *m_tli);
      InterBlockBuilder interBuilder (F, *g, *m_dl, *m_tli);
      for (const BasicBlock *bb : bbs)
        intraBuilder.visit (*const_cast<BasicBlock*>(bb));
      for (const BasicBlock *bb : bbs)
        interBuilder.visit (*const_cast<BasicBlock*>(bb));
      
      LOG ("dsa-local", 
           errs () << "Dsa graph after " << F.getName () << "\n";
           g->write(errs()));
      
      m_graphs [&F] = g;
      return false;
    }

    bool Local::hasGraph (const Function& F) const {
      return (m_graphs.find(&F) != m_graphs.end());
    }

    const Graph& Local::getGraph (const Function& F) const {
      auto it = m_graphs.find(&F);
      assert (it != m_graphs.end());
      return *(it->second);
    }

    //Pass * createDsaLocalPass () {return new Local ();}
  }
}

char seahorn::dsa::Local::ID = 0;
