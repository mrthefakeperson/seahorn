// Symbolic execution (loosely) based on semantics used in UFO
#include "llvm/IR/GetElementPtrTypeIterator.h"

#include "seahorn/BvSymExec.hh"
#include "seahorn/Support/CFG.hh"
#include "seahorn/Transforms/Instrumentation/ShadowMemDsa.hh"

#include "ufo/ufo_iterators.hpp"
#include "llvm/Support/CommandLine.h"

//#include <queue>

using namespace seahorn;
using namespace llvm;
using namespace ufo;

static llvm::cl::opt<bool>
GlobalConstraints("bv-horn-global-constraints",
                  llvm::cl::desc
                  ("Maximize the use of global (i.e., unguarded) constraints"),
                  cl::init (false));

static llvm::cl::opt<bool>
ArrayGlobalConstraints("bv-horn-array-global-constraints",
                       llvm::cl::desc
                       ("Extend global constraints to arrays"),
                       cl::init (false));

static llvm::cl::opt<bool>
EnableUniqueScalars ("bv-horn-singleton-aliases",
                     llvm::cl::desc ("Treat singleton alias sets as scalar values"),
                     cl::init (true));

static llvm::cl::opt<bool>
InferMemSafety ("bv-horn-use-mem-safety",
                llvm::cl::desc ("Rely on memory safety assumptions such as "
                                "successful load/store imply validity of their arguments"),
                cl::init(true),
                cl::Hidden);

static llvm::cl::opt<bool>
IgnoreCalloc ("bv-horn-ignore-calloc",
              llvm::cl::desc ("Treat calloc same as malloc, ignore that memory is initialized"),
              cl::init (false),
              cl::Hidden);


static const Value *extractUniqueScalar (CallSite &cs)
{
   if (!EnableUniqueScalars) 
     return nullptr;
   else
     return seahorn::shadow_dsa::extractUniqueScalar (cs);
}

static const Value* extractUniqueScalar (const CallInst *ci)
{
   if (!EnableUniqueScalars) 
     return nullptr;
   else 
     return seahorn::shadow_dsa::extractUniqueScalar (ci);
}

static bool isShadowMem (const Value &V, const Value **out) 
{
   const Value *scalar;
   bool res = seahorn::shadow_dsa::isShadowMem (V, &scalar);
   if (EnableUniqueScalars && out) *out = scalar;
   return res;
}

namespace
{
  struct SymExecBase
  {
    SymStore &m_s;
    ExprFactory &m_efac;
    BvSmallSymExec &m_sem;
    ExprVector &m_side;
    
    Expr trueE;
    Expr falseE;
    Expr zeroE;
    Expr oneE;
    Expr trueBv;
    Expr falseBv;
    Expr nullBv;
    
    /// -- current read memory
    Expr m_inMem;
    /// -- current write memory
    Expr m_outMem;
    /// --- true if the current read/write is to unique memory location
    bool m_uniq;
    
    /// -- parameters for a function call
    ExprVector m_fparams;
    
    Expr m_activeLit;
    
    SymExecBase (SymStore &s, BvSmallSymExec &sem, ExprVector &side) : 
      m_s(s), m_efac (m_s.getExprFactory ()), m_sem (sem), m_side (side) 
    {
      trueE = mk<TRUE> (m_efac);
      falseE = mk<FALSE> (m_efac);

      zeroE = mkTerm<mpz_class> (0, m_efac);
      oneE = mkTerm<mpz_class> (1, m_efac);
      trueBv = bv::bvnum (1, 1, m_efac);
      falseBv = bv::bvnum (0, 1, m_efac);
      nullBv = bv::bvnum (0, m_sem.pointerSizeInBits (), m_efac);
      m_uniq = false;
      resetActiveLit ();
      // -- first two arguments are reserved for error flag
      m_fparams.push_back (falseE);
      m_fparams.push_back (falseE);
      m_fparams.push_back (falseE);
    }
    Expr symb (const Value &I) {return m_sem.symb (I);}
    
    Expr read (const Value &v)
    {
      return m_sem.isTracked (v) ? m_s.read (symb (v)) : Expr (0);
    }

    Expr lookup (const Value &v) {return m_sem.lookup (m_s, v);}
    Expr havoc (const Value &v) 
    {return m_sem.isTracked (v) ? m_s.havoc (symb (v)) : Expr (0);}

    void resetActiveLit () {m_activeLit = trueE;}
    void setActiveLit (Expr act) {m_activeLit = act;}
    
    // -- add conditional side condition
    void addCondSide (Expr c) {side (c, true);}
    
    
    void side (Expr v, bool conditional = false)
    {
      if (!v) return;
      if (GlobalConstraints || conditional)
        m_side.push_back (boolop::limp (m_activeLit, v));
      else
        m_side.push_back (v);
    }
   
    void bside (Expr lhs, Expr rhs, bool conditional = false)
    {if (lhs && rhs) side (mk<IFF> (lhs, rhs), conditional);}
    
   
    void side (Expr lhs, Expr rhs, bool conditional = false)
    {if (lhs && rhs) side (mk<EQ> (lhs, rhs), conditional);}

    /// convert bv1 to bool
    Expr bvToBool (Expr bv) {return mk<EQ> (bv, trueBv);}
    
  };
  
  struct SymExecVisitor : public InstVisitor<SymExecVisitor>, 
                          SymExecBase
  {
    SymExecVisitor (SymStore &s, BvSmallSymExec &sem, ExprVector &side) : 
      SymExecBase (s, sem, side) {}
    
    /// base case. if all else fails.
    void visitInstruction (Instruction &I) {havoc (I);}
    
    /// skip PHI nodes
    void visitPHINode (PHINode &I) { /* do nothing */ }
    
    void visitCmpInst (CmpInst &I)
    {
      Expr lhs = havoc (I);
      
      const Value& v0 = *I.getOperand (0);
      const Value& v1 = *I.getOperand (1);
      
      Expr op0 = lookup (v0);
      Expr op1 = lookup (v1);

      if (!(op0 && op1)) return;

      Expr rhs;
      
      switch (I.getPredicate ())
      {
      case CmpInst::ICMP_EQ:
        rhs = mk<EQ> (op0, op1);
        break;
      case CmpInst::ICMP_NE:
        rhs = mk<NEQ> (op0, op1);
        break;
      case CmpInst::ICMP_UGT:
        rhs = mk<BUGT> (op0, op1);
        break;
      case CmpInst::ICMP_SGT:
        if (v0.getType ()->isIntegerTy (1))
        {
          if (isOpX<TRUE> (op1))
            // icmp sgt op0, i1 true  == !op0
            rhs = boolop::lneg (op0);
          // XXX handle other cases 
        }
        else
          rhs = mk<BSGT> (op0, op1);
        break;
      case CmpInst::ICMP_UGE:
        rhs = mk<BUGE> (op0, op1);
        break;
      case CmpInst::ICMP_SGE:
        rhs = mk<BSGE> (op0, op1);
        break;
      case CmpInst::ICMP_ULT:
        rhs = mk<BULT> (op0, op1);
        break;
      case CmpInst::ICMP_SLT:
        rhs = mk<BSLT> (op0, op1);
        break; 
      case CmpInst::ICMP_ULE:
        rhs = mk<BULE> (op0, op1);
        break;
      case CmpInst::ICMP_SLE:
        rhs = mk<BSLE> (op0, op1);
        break;
      default:
        break;
      }       

      bside (lhs, rhs);
    }
    
    
    void visitSelectInst(SelectInst &I)
    {
      if (!m_sem.isTracked (I)) return;
      
      Expr lhs = havoc (I);
      Expr cond = lookup (*I.getCondition ());
      Expr op0 = lookup (*I.getTrueValue ());
      Expr op1 = lookup (*I.getFalseValue ());
      
      if (cond && op0 && op1) side (lhs, mk<ITE> (cond, op0, op1));
    }
    
    void visitBinaryOperator(BinaryOperator &I)
    {
      if (!m_sem.isTracked (I)) return;
      
      const Value& v0 = *(I.getOperand (0));
      const Value& v1 = *(I.getOperand (1));

      Expr op0 = lookup (v0);
      Expr op1 = lookup (v1);
      if (!(op0 && op1)) return;

      Expr lhs = havoc (I);
      Expr rhs;
      switch (I.getOpcode ())
      {
      case BinaryOperator::Add:
        rhs = mk<BADD> (op0, op1);
        break;
      case BinaryOperator::Sub:
        rhs = mk<BSUB> (op0, op1);
        break;
      case BinaryOperator::Mul:
        rhs = mk<BMUL> (op0, op1);
        break;
      case BinaryOperator::UDiv:
        rhs = mk<BUDIV> (op0, op1);
        break;
      case BinaryOperator::SDiv:
        rhs = mk<BSDIV> (op0, op1);
        break;
      case BinaryOperator::Shl:
        rhs = mk<BSHL> (op0, op1);
        break;
      case BinaryOperator::AShr:
        rhs = mk<BASHR> (op0, op1);
        break;
      case BinaryOperator::SRem:
        rhs = mk<BSREM> (op0, op1);
        break;
      case BinaryOperator::URem:
        rhs = mk<BUREM> (op0, op1);
        break;
      case BinaryOperator::And:
        if (v0.getType ()->isIntegerTy (1) && v1.getType ()->isIntegerTy (1))
          rhs = mk<AND> (op0, op1);
        else
          rhs = mk<BAND> (op0, op1);
        break;
      case BinaryOperator::Or:
        if (v0.getType ()->isIntegerTy (1) && v1.getType ()->isIntegerTy (1))
          rhs = mk<OR> (op0, op1);
        else
          rhs = mk<BOR> (op0, op1);
        break;
      case BinaryOperator::Xor:
        if (v0.getType ()->isIntegerTy (1) && v1.getType ()->isIntegerTy (1))
          rhs = mk<XOR> (op0, op1);
        else
          rhs = mk<BXOR> (op0, op1);
        break;
      case BinaryOperator::LShr:
        rhs = mk<BLSHR> (op0, op1);
        break;
      default:
        break;
      }

      side (lhs, rhs);
    }
    
    void visitReturnInst (ReturnInst &I)
    {
      // -- skip return argument of main
      if (I.getParent ()->getParent ()->getName ().equals ("main")) return;
      
      if (I.getNumOperands () > 0)
        lookup (*I.getOperand (0));
    }
    
    void visitBranchInst (BranchInst &I)
    {
      if (I.isConditional ()) lookup (*I.getCondition ());
    }

    void visitTruncInst(TruncInst &I)              
    {
      if (!m_sem.isTracked (I)) return;
      Expr lhs = havoc (I);
      Expr op0 = lookup (*I.getOperand (0));
      
      if (!op0) return;
      
      uint64_t width = m_sem.sizeInBits (I);
      Expr rhs = bv::extract (width-1, 0, op0);
      
      if (I.getType ()->isIntegerTy (1)) rhs = bvToBool (rhs);
      side (lhs, rhs);
    }
    
    void visitZExtInst (ZExtInst &I) 
    {
      if (!m_sem.isTracked (I)) return;
      Expr lhs = havoc (I);
      Expr op0 = lookup (*I.getOperand (0));
      if (!op0) return;
      
      Expr rhs = bv::zext (op0, m_sem.sizeInBits (I));
      side (lhs, rhs);
    }
    void visitSExtInst (SExtInst &I) 
    {
      if (!m_sem.isTracked (I)) return;
      Expr lhs = havoc (I);
      Expr op0 = lookup (*I.getOperand (0));
      if (!op0) return;
      
      Expr rhs = bv::sext (op0, m_sem.sizeInBits (I));
      side (lhs, rhs);
    }
    
    void off_visitGetElementPtrInst (GetElementPtrInst &gep)
    {
      
      if (!m_sem.isTracked (gep)) return;
      Expr lhs = havoc (gep);
      
      SmallVector<const Value*, 4> ps;
      SmallVector<const Type*, 4> ts;
      gep_type_iterator typeIt = gep_type_begin (gep);
      for (unsigned i = 1; i < gep.getNumOperands (); ++i, ++typeIt)
      {
        ps.push_back (gep.getOperand (i));
        ts.push_back (*typeIt);
      }
      
      Expr op = m_sem.ptrArith (m_s, *gep.getPointerOperand (), ps, ts);
      Expr act = GlobalConstraints ? trueE : m_activeLit;
      if (op)
      {
        m_side.push_back (boolop::limp (act, mk<EQ> (lhs, op)));
        if (!InferMemSafety) return;
        
        // -- extra constraints that exclude undefined behavior
        if (!gep.isInBounds () || gep.getPointerAddressSpace () != 0)
          return;
        if (Expr base = lookup (*gep.getPointerOperand ()))
          // -- base > 0 -> lhs > 0
          m_side.push_back (boolop::limp (m_activeLit,
                                          mk<OR> (mk<LEQ> (base, zeroE),
                                                  mk<GT> (lhs, zeroE))));
      }
      
    }
    
    void visitCallSite (CallSite CS)
    {
      assert (CS.isCall ());
      const Function *f = CS.getCalledFunction ();
      
      Instruction &I = *CS.getInstruction ();
      BasicBlock &BB = *I.getParent ();
      
      // -- unknown/indirect function call
      if (!f)
      { 
        // XXX Use DSA and/or Devirt to handle better
        assert (m_fparams.size () == 3);
        visitInstruction (I);
        return;
      }
      
      const Function &F = *f;
      const Function &PF = *I.getParent ()->getParent ();
      
      // skip intrinsic functions
      if (F.isIntrinsic ()) { assert (m_fparams.size () == 3); return;}
    
      
      if (F.getName ().startswith ("verifier.assume"))
      {
        Expr c = lookup (*CS.getArgument (0));
        if (F.getName ().equals ("verifier.assume.not")) c = boolop::lneg (c);
        
        assert (m_fparams.size () == 3);
        // -- assumption is only active when error flag is false
        addCondSide (boolop::lor (m_s.read (m_sem.errorFlag (BB)), c));
      }
      else if (F.getName ().equals ("calloc") && m_inMem && m_outMem && m_sem.isTracked (I))
      {
        havoc (I);
        assert (m_fparams.size () == 3);
        assert (!m_uniq);
      }
      
      else if (m_sem.hasFunctionInfo (F))
      {
        const FunctionInfo &fi = m_sem.getFunctionInfo (F);
        
        // enabled
        m_fparams [0] = m_activeLit; // activation literal
        // error flag in
        m_fparams [1] = (m_s.read (m_sem.errorFlag (BB)));
        // error flag out
        m_fparams [2] = (m_s.havoc (m_sem.errorFlag (BB)));
        for (const Argument *arg : fi.args)
          m_fparams.push_back (m_s.read (symb (*CS.getArgument (arg->getArgNo ()))));
        for (const GlobalVariable *gv : fi.globals)
          m_fparams.push_back (m_s.read (symb (*gv)));
        
        if (fi.ret) m_fparams.push_back (m_s.havoc (symb (I)));
        
        LOG ("arg_error", 
             if (m_fparams.size () != bind::domainSz (fi.sumPred))
             {
               errs () << "Call instruction: " << I << "\n";
               errs () << "Caller: " << PF << "\n";
               errs () << "Callee: " << F << "\n";
               // errs () << "Sum predicate: " << *fi.sumPred << "\n";
               errs () << "m_fparams.size: " << m_fparams.size () << "\n";
               errs () << "Domain size: " << bind::domainSz (fi.sumPred) << "\n";
               errs () << "m_fparams\n";
               for (auto r : m_fparams) errs () << *r << "\n";
               errs () << "regions: " << fi.regions.size () 
                       << " args: " << fi.args.size () 
                       << " globals: " << fi.globals.size () 
                       << " ret: " << fi.ret << "\n";
               errs () << "regions\n";
               for (auto r : fi.regions) errs () << *r << "\n";
               errs () << "args\n";
               for (auto r : fi.args) errs () << *r << "\n";
               errs () << "globals\n";
               for (auto r : fi.globals) errs () << *r << "\n";
               if (fi.ret) errs () << "ret: " << *fi.ret << "\n";
             }
             );
        
        assert (m_fparams.size () == bind::domainSz (fi.sumPred));
        m_side.push_back (bind::fapp (fi.sumPred, m_fparams));
        
        m_fparams.clear ();
        m_fparams.push_back (falseE);
        m_fparams.push_back (falseE);
        m_fparams.push_back (falseE);
      }
      else if (F.getName ().startswith ("shadow.mem") && 
               m_sem.isTracked (I))
      {
        if (F.getName ().equals ("shadow.mem.init"))
          m_s.havoc (symb(I));
        else if (F.getName ().equals ("shadow.mem.load"))
        {
          const Value &v = *CS.getArgument (1);
          m_inMem = m_s.read (symb (v));
          m_uniq = extractUniqueScalar (CS) != nullptr;
        }
        else if (F.getName ().equals ("shadow.mem.store"))
        {
          m_inMem = m_s.read (symb (*CS.getArgument (1)));
          m_outMem = m_s.havoc (symb (I));
          m_uniq = extractUniqueScalar (CS) != nullptr;
        }
        else if (F.getName ().equals ("shadow.mem.arg.ref"))
          m_fparams.push_back (m_s.read (symb (*CS.getArgument (1))));
        else if (F.getName ().equals ("shadow.mem.arg.mod"))
        {
          m_fparams.push_back (m_s.read (symb (*CS.getArgument (1))));
          m_fparams.push_back (m_s.havoc (symb (I)));
        }
        else if (F.getName ().equals ("shadow.mem.arg.new"))
          m_fparams.push_back (m_s.havoc (symb (I)));
        else if (!PF.getName ().equals ("main") && 
                 F.getName ().equals ("shadow.mem.in"))
        {
          m_s.read (symb (*CS.getArgument (1)));
        }
        else if (!PF.getName ().equals ("main") &&
                 F.getName ().equals ("shadow.mem.out"))
        {
          m_s.read (symb (*CS.getArgument (1)));
        }
        else if (!PF.getName ().equals ("main") && 
                 F.getName ().equals ("shadow.mem.arg.init"))
        {
          // regions initialized in main are global. We want them to
          // flow to the arguments
          /* do nothing */
        }
      }
      else
      {
        if (m_fparams.size () > 3)
        {
          m_fparams.resize (3);
          errs () << "WARNING: skipping a call to " << F.getName () 
                  << " (recursive call?)\n";
        }
        
        visitInstruction (*CS.getInstruction ());
      }
          
    }
    
    void visitAllocaInst (AllocaInst &I)
    {
      if (!m_sem.isTracked (I)) return;
      
      Expr lhs = havoc(I);
      // -- alloca always returns a non-zero address
      side (mk<BUGT> (lhs, nullBv));
    }
    
    void visitLoadInst (LoadInst &I)
    {
      if (InferMemSafety)
      {
        Value *pop = I.getPointerOperand ()->stripPointerCasts ();
        // -- successful load through a gep implies that the base
        // -- address of the gep is not null
        if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst> (pop))
        {
          Expr base = lookup (*gep->getPointerOperand ());
          if (base) side (mk<BUGT> (base, nullBv), true);
        }
      }
      
      if (!m_sem.isTracked (I))
      {
        m_inMem.reset ();
        return;
      }
      
      
      m_inMem.reset ();
    }
    
    
    void visitStoreInst (StoreInst &I)
    {
      if (InferMemSafety)
      {
        Value *pop = I.getPointerOperand ()->stripPointerCasts ();
        // -- successful load through a gep implies that the base
        // -- address of the gep is not null
        if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst> (pop))
        {
          Expr base = lookup (*gep->getPointerOperand ());
          if (base) side (mk<BUGT> (base, nullBv));
        }
      }

      if (!m_inMem || !m_outMem || !m_sem.isTracked (*I.getOperand (0)))
      {
        m_inMem.reset ();
        m_outMem.reset ();
        return;
      }
          
      m_inMem.reset ();
      m_outMem.reset ();
    }
    
    
    void visitCastInst (CastInst &I)
    {
      if (!m_sem.isTracked (I)) return;

      Expr lhs = havoc (I);
      const Value &v0 = *I.getOperand (0);
      Expr u = lookup (v0);

      // -- what can this be? Might need to do something here.
      side (lhs, lookup (v0));
    }
    
    void initGlobals (const BasicBlock &BB)
    {
      const Function &F = *BB.getParent ();
      if (&F.getEntryBlock () != &BB) return;
      if (!F.getName ().equals ("main")) return;
      
      const Module &M = *F.getParent ();
      for (const GlobalVariable &g : boost::make_iterator_range (M.global_begin (),
                                                                 M.global_end ()))
        if (m_sem.isTracked (g)) havoc (g);
    }
    
    void visitBasicBlock (BasicBlock &BB)
    {
      /// -- check if globals need to be initialized
      initGlobals (BB);
      
      // read the error flag to make it live
      m_s.read (m_sem.errorFlag (BB));
    }
    
  };
    
  struct SymExecPhiVisitor : public InstVisitor<SymExecPhiVisitor>,
                             SymExecBase
  {
    const BasicBlock &m_dst;
    
    SymExecPhiVisitor (SymStore &s, BvSmallSymExec &sem, 
                       ExprVector &side, const BasicBlock &dst) : 
      SymExecBase (s, sem, side), m_dst (dst) {}

    void visitBasicBlock (BasicBlock &BB)
    {
      // -- evaluate all phi-nodes atomically. First read all incoming
      // -- values, then update phi-nodes all together.
      ExprVector ops;

      auto curr = BB.begin ();
      if (!isa<PHINode> (curr)) return;
      
      for (; PHINode *phi = dyn_cast<PHINode> (curr); ++curr)
      {
        // skip phi nodes that are not tracked
        if (!m_sem.isTracked (*phi)) continue;
        const Value &v = *phi->getIncomingValueForBlock (&m_dst);
        ops.push_back (lookup (v));
      }
      
      curr = BB.begin ();
      for (unsigned i = 0; isa<PHINode> (curr); ++curr)
      {
        PHINode &phi = *cast<PHINode> (curr);
        if (!m_sem.isTracked (phi)) continue;
        Expr lhs = havoc (phi);
        Expr op0 = ops[i++];
        side (lhs, op0);
      }
    }
  };
}

namespace seahorn
{
  Expr BvSmallSymExec::errorFlag (const BasicBlock &BB)
  {
    // -- if BB belongs to a function that cannot fail, errorFlag is always false
    if (m_canFail && !m_canFail->canFail (BB.getParent ())) return falseE;
    return this->SmallStepSymExec::errorFlag (BB);
  }
  
  void BvSmallSymExec::exec (SymStore &s, const BasicBlock &bb, ExprVector &side,
                              Expr act)
  {
    SymExecVisitor v(s, *this, side);
    v.setActiveLit (act);
    v.visit (const_cast<BasicBlock&>(bb));
    v.resetActiveLit ();
  }
    
  void BvSmallSymExec::exec (SymStore &s, const Instruction &inst, ExprVector &side)
  {
    SymExecVisitor v (s, *this, side);
    v.visit (const_cast<Instruction&>(inst));
  }
    
  
  void BvSmallSymExec::execPhi (SymStore &s, const BasicBlock &bb, 
                                 const BasicBlock &from, ExprVector &side, Expr act)
  {
    // act is ignored since phi node only introduces a definition
    SymExecPhiVisitor v(s, *this, side, from);
    v.setActiveLit (act);
    v.visit (const_cast<BasicBlock&>(bb));
    v.resetActiveLit ();
  }

  Expr BvSmallSymExec::ptrArith (SymStore &s, 
                                  const Value &base,
                                  SmallVectorImpl<const Value*> &ps,
                                  SmallVectorImpl<const Type*> &ts)
  {
    Expr res = lookup (s, base);
    if (!res) return res;
    
    for (unsigned i = 0; i < ps.size (); ++i)
    {
      if (const StructType *st = dyn_cast<const StructType> (ts [i]))
      {
        if (const ConstantInt *ci = dyn_cast<const ConstantInt> (ps [i]))
        {
          Expr off = mkTerm<mpz_class> (fieldOff (st, ci->getZExtValue ()), m_efac);
          res = mk<PLUS> (res, off);
        }
        else assert (0);
      }
      else if (const SequentialType *seqt = dyn_cast<const SequentialType> (ts [i]))
      {
        Expr sz = mkTerm<mpz_class> (storageSize (seqt->getElementType ()), m_efac);
        res = mk<PLUS> (res, mk<MULT> (lookup (s, *ps[i]), sz));
      }
    }
    return res;
  }
  
  unsigned BvSmallSymExec::pointerSizeInBits () const
  {return m_td->getPointerSizeInBits ();}
  
  uint64_t BvSmallSymExec::sizeInBits (const llvm::Type &t) const
  {return m_td->getTypeSizeInBits (const_cast<llvm::Type*> (&t));} 

  uint64_t BvSmallSymExec::sizeInBits (const llvm::Value &v) const
  {return sizeInBits (*v.getType ());}
  
  
  unsigned BvSmallSymExec::storageSize (const llvm::Type *t) 
  {return m_td->getTypeStoreSize (const_cast<Type*> (t));}
  
  unsigned BvSmallSymExec::fieldOff (const StructType *t, unsigned field)
  {
    return m_td->getStructLayout (const_cast<StructType*>(t))->getElementOffset (field);
  }
    
  Expr BvSmallSymExec::symb (const Value &I)
  {
    assert (!isa<UndefValue>(&I));

    // -- basic blocks are mapped to Bool constants
    if (const BasicBlock *bb = dyn_cast<const BasicBlock> (&I))
      return bind::boolConst 
        (mkTerm<const BasicBlock*> (bb, m_efac));
    
    // -- constants are mapped to values
    if (const Constant *cv = dyn_cast<const Constant> (&I))
    {
      if (const ConstantInt *c = dyn_cast<const ConstantInt> (&I))
      {
        if (c->getType ()->isIntegerTy (1))
          return c->isOne () ? mk<TRUE> (m_efac) : mk<FALSE> (m_efac);
        mpz_class k = toMpz (c->getValue ());
        return bv::bvnum (k, sizeInBits (I), m_efac);
      }
      else if (cv->isNullValue () || isa<ConstantPointerNull> (&I))
        return bv::bvnum (0, sizeInBits (*cv), m_efac);
      else if (const ConstantExpr *ce = dyn_cast<const ConstantExpr> (&I))
      {
        // XXX Need a better handling of constant expressions
        // XXX Perhaps fold using constant folding first, than evaluate the result
        // -- if this is a cast, and not into a Boolean, strip it
        if (ce->isCast () && 
            (ce->getType ()->isIntegerTy () || ce->getType ()->isPointerTy ()) && 
            ! ce->getType ()->isIntegerTy (1))
   
        {
          if (const ConstantInt* val = dyn_cast<const ConstantInt>(ce->getOperand (0)))
          {
            mpz_class k = toMpz (val->getValue ());
            return bv::bvnum (k, sizeInBits (I), m_efac);
          }
          // -- strip cast
          else return symb (*ce->getOperand (0));
        }
      }
    }   
     
    // -- everything else is mapped to a constant
    Expr v = mkTerm<const Value*> (&I, m_efac);
    
    const Value *scalar = nullptr;
    if (isShadowMem (I, &scalar))
    {
      if (scalar)
        // -- create a constant with the name v[scalar]
        return bv::bvConst
          (op::array::select (v, mkTerm<const Value*> (scalar, m_efac)),
           sizeInBits (*scalar));
    
      if (m_trackLvl >= MEM)
      {
        Expr ptrTy = bv::bvsort (pointerSizeInBits (), m_efac);
        Expr valTy = ptrTy;
        Expr memTy = sort::arrayTy (ptrTy, valTy);
        return bind::mkConst (v, memTy);
      }
    }
    
      
    if (isTracked (I))
      return I.getType ()->isIntegerTy (1) ? 
        bind::boolConst (v) : bv::bvConst (v, sizeInBits (I));
    
    
    return Expr(0);
  }
  
  const Value &BvSmallSymExec::conc (Expr v)
  {
    assert (isOpX<FAPP> (v));
    // name of the app
    Expr u = bind::fname (v);
    // name of the fdecl
    u = bind::fname (u);
    assert (isOpX<VALUE> (v));
    return *getTerm<const Value*> (v);
  }
  
  
  bool BvSmallSymExec::isTracked (const Value &v) 
  {
    const Value* scalar;
    
    // -- shadow values represent memory regions
    // -- only track them when memory is tracked
    if (isShadowMem (v, &scalar))
      return scalar != nullptr || m_trackLvl >= MEM;
    
    
    // -- a pointer
    if (v.getType ()->isPointerTy ())
    {
      // -- XXX A hack because shadow.mem generates not well formed
      // -- bitcode that contains future references. A register that
      // -- is defined later is used to name a shadow region in the
      // -- beginning of the function. Perhaps there is a better
      // -- solution. For now, we just do not track anything that came
      // -- that way.
      if (v.hasOneUse ())
        if (const CallInst *ci = dyn_cast<const CallInst> (*v.user_begin ()))
          if (const Function *fn = ci->getCalledFunction ())
            if (fn->getName ().startswith ("shadow.mem")) return false;
      
      return m_trackLvl >= PTR;
    }
    
    
    // -- always track integer registers
    return v.getType ()->isIntegerTy ();
  }
  
  Expr BvSmallSymExec::lookup (SymStore &s, const Value &v)
  {
    Expr u = symb (v);
    // if u is defined it is either an fapp or a constant
    if (u) return bind::isFapp (u) ? s.read (u) : u;
    return Expr (0);
  }

  void BvSmallSymExec::execEdg (SymStore &s, const BasicBlock &src,
                                 const BasicBlock &dst, ExprVector &side)
  {
    exec (s, src, side, trueE);
    execBr (s, src, dst, side, trueE);
    execPhi (s, dst, src, side, trueE);
    
    // an edge into a basic block that does not return includes the block itself
    const TerminatorInst *term = dst.getTerminator ();
    if (term && isa<const UnreachableInst> (term)) exec (s, dst, side, trueE);
  }
  
  void BvSmallSymExec::execBr (SymStore &s, const BasicBlock &src,
                               const BasicBlock &dst, 
                               ExprVector &side, Expr act)
  {
    // the branch condition
    if (const BranchInst *br = dyn_cast<const BranchInst> (src.getTerminator ()))
    {
      if (br->isConditional ())
      {
        const Value &c = *br->getCondition ();
        if (const ConstantInt *ci = dyn_cast<const ConstantInt> (&c))
        {
          if ((ci->isOne () && br->getSuccessor (0) != &dst) ||
              (ci->isZero () && br->getSuccessor (1) != &dst))
          {
            side.clear ();
            side.push_back (boolop::limp (act, s.read (errorFlag (src))));
          }
        }
        else if (Expr target = lookup (s, c))
        {
          
          Expr cond = br->getSuccessor (0) == &dst ? target : mk<NEG> (target);
          cond = boolop::lor (s.read (errorFlag (src)), cond);
          cond = boolop::limp (act, cond);
          side.push_back (cond);
        }
        
            
      }
    }   
  }
}