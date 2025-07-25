// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                           Lowering for AMD64, x86                         XX
XX                                                                           XX
XX  This encapsulates all the logic for lowering trees for the AMD64         XX
XX  architecture.  For a more detailed view of what is lowering, please      XX
XX  take a look at Lower.cpp                                                 XX
XX                                                                           XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif

#ifdef TARGET_XARCH // This file is only used for xarch

#include "jit.h"
#include "sideeffects.h"
#include "lower.h"

// xarch supports both ROL and ROR instructions so no lowering is required.
void Lowering::LowerRotate(GenTree* tree)
{
    ContainCheckShiftRotate(tree->AsOp());
}

//------------------------------------------------------------------------
// LowerStoreLoc: Lower a store of a lclVar
//
// Arguments:
//    storeLoc - the local store (GT_STORE_LCL_FLD or GT_STORE_LCL_VAR)
//
// Notes:
//    This involves:
//    - Handling of contained immediates.
//    - Widening some small stores.
//
// Returns:
//   Next tree to lower.
//
GenTree* Lowering::LowerStoreLoc(GenTreeLclVarCommon* storeLoc)
{
    // Most small locals (the exception is dependently promoted fields) have 4 byte wide stack slots, so
    // we can widen the store, if profitable. The widening is only (largely) profitable for 2 byte stores.
    // We could widen bytes too but that would only be better when the constant is zero and reused, which
    // we presume is not common enough.
    //
    if (storeLoc->OperIs(GT_STORE_LCL_VAR) && (genTypeSize(storeLoc) == 2) && storeLoc->Data()->IsCnsIntOrI())
    {
        if (!comp->lvaGetDesc(storeLoc)->lvIsStructField)
        {
            storeLoc->gtType = TYP_INT;
        }
    }
    if (storeLoc->OperIs(GT_STORE_LCL_FLD))
    {
        // We should only encounter this for lclVars that are lvDoNotEnregister.
        verifyLclFldDoNotEnregister(storeLoc->GetLclNum());
    }

    ContainCheckStoreLoc(storeLoc);
    return storeLoc->gtNext;
}

//------------------------------------------------------------------------
// LowerStoreIndir: Determine addressing mode for an indirection, and whether operands are contained.
//
// Arguments:
//    node       - The indirect store node (GT_STORE_IND) of interest
//
// Return Value:
//    Next node to lower.
//
GenTree* Lowering::LowerStoreIndir(GenTreeStoreInd* node)
{
    // Mark all GT_STOREIND nodes to indicate that it is not known
    // whether it represents a RMW memory op.
    node->SetRMWStatusDefault();

    if (!varTypeIsFloating(node))
    {
        // Perform recognition of trees with the following structure:
        //        StoreInd(addr, BinOp(expr, GT_IND(addr)))
        // to be able to fold this into an instruction of the form
        //        BINOP [addr], register
        // where register is the actual place where 'expr' is computed.
        //
        // SSE2 doesn't support RMW form of instructions.
        if (LowerRMWMemOp(node))
        {
            return node->gtNext;
        }
    }

    // Optimization: do not unnecessarily zero-extend the result of setcc.
    if (varTypeIsByte(node) && (node->Data()->OperIsCompare() || node->Data()->OperIs(GT_SETCC)))
    {
        node->Data()->ChangeType(TYP_BYTE);
    }
    ContainCheckStoreIndir(node);

    return node->gtNext;
}

//----------------------------------------------------------------------------------------------
// Lowering::TryLowerMulWithConstant:
//    Lowers a tree MUL(X, CNS) to LSH(X, CNS_SHIFT)
//    or
//    Lowers a tree MUL(X, CNS) to SUB(LSH(X, CNS_SHIFT), X)
//    or
//    Lowers a tree MUL(X, CNS) to ADD(LSH(X, CNS_SHIFT), X)
//
// Arguments:
//    node - GT_MUL node of integral type
//
// Return Value:
//    Returns the replacement node if one is created else nullptr indicating no replacement
//
// Notes:
//    Performs containment checks on the replacement node if one is created
GenTree* Lowering::TryLowerMulWithConstant(GenTreeOp* node)
{
    assert(node->OperIs(GT_MUL));

    // Do not do these optimizations when min-opts enabled.
    if (comp->opts.MinOpts())
        return nullptr;

    if (!varTypeIsIntegral(node))
        return nullptr;

    if (node->gtOverflow())
        return nullptr;

    GenTree* op1 = node->gtGetOp1();
    GenTree* op2 = node->gtGetOp2();

    if (op1->isContained() || op2->isContained())
        return nullptr;

    if (!op2->IsCnsIntOrI())
        return nullptr;

    GenTreeIntConCommon* cns    = op2->AsIntConCommon();
    ssize_t              cnsVal = cns->IconValue();

    // Use GT_LEA if cnsVal is 3, 5, or 9.
    // These are handled in codegen.
    if (cnsVal == 3 || cnsVal == 5 || cnsVal == 9)
        return nullptr;

    // Use GT_LSH if cnsVal is a power of two.
    if (isPow2(cnsVal))
    {
        // Use shift for constant multiply when legal
        unsigned int shiftAmount = genLog2(static_cast<uint64_t>(static_cast<size_t>(cnsVal)));

        cns->SetIconValue(shiftAmount);
        node->ChangeOper(GT_LSH);

        ContainCheckShiftRotate(node);

        return node;
    }

// We do not do this optimization in X86 as it is not recommended.
#if TARGET_X86
    return nullptr;
#endif // TARGET_X86

    ssize_t cnsValPlusOne  = cnsVal + 1;
    ssize_t cnsValMinusOne = cnsVal - 1;

    bool useSub = isPow2(cnsValPlusOne);

    if (!useSub && !isPow2(cnsValMinusOne))
        return nullptr;

    LIR::Use op1Use(BlockRange(), &node->gtOp1, node);
    op1 = ReplaceWithLclVar(op1Use);

    if (useSub)
    {
        cnsVal = cnsValPlusOne;
        node->ChangeOper(GT_SUB);
    }
    else
    {
        cnsVal = cnsValMinusOne;
        node->ChangeOper(GT_ADD);
    }

    unsigned int shiftAmount = genLog2(static_cast<uint64_t>(static_cast<size_t>(cnsVal)));
    cns->SetIconValue(shiftAmount);

    node->gtOp1 = comp->gtNewOperNode(GT_LSH, node->gtType, op1, cns);
    node->gtOp2 = comp->gtClone(op1);

    BlockRange().Remove(op1);
    BlockRange().Remove(cns);
    BlockRange().InsertBefore(node, node->gtGetOp2());
    BlockRange().InsertBefore(node, cns);
    BlockRange().InsertBefore(node, op1);
    BlockRange().InsertBefore(node, node->gtGetOp1());

    ContainCheckBinary(node);
    ContainCheckShiftRotate(node->gtGetOp1()->AsOp());

    return node;
}

//------------------------------------------------------------------------
// LowerMul: Lower a GT_MUL/GT_MULHI/GT_MUL_LONG node.
//
// Currently only performs containment checks.
//
// Arguments:
//    mul - The node to lower
//
// Return Value:
//    The next node to lower.
//
GenTree* Lowering::LowerMul(GenTreeOp* mul)
{
    assert(mul->OperIsMul());

    if (mul->OperIs(GT_MUL))
    {
        GenTree* replacementNode = TryLowerMulWithConstant(mul);
        if (replacementNode != nullptr)
        {
            return replacementNode->gtNext;
        }
    }

    ContainCheckMul(mul);

    return mul->gtNext;
}

//------------------------------------------------------------------------
// LowerBinaryArithmetic: lowers the given binary arithmetic node.
//
// Recognizes opportunities for using target-independent "combined" nodes
// Performs containment checks.
//
// Arguments:
//    node - the arithmetic node to lower
//
// Returns:
//    The next node to lower.
//
GenTree* Lowering::LowerBinaryArithmetic(GenTreeOp* binOp)
{
#ifdef FEATURE_HW_INTRINSICS
    if (comp->opts.OptimizationEnabled() && varTypeIsIntegral(binOp))
    {
        if (binOp->OperIs(GT_AND))
        {
            GenTree* replacementNode = TryLowerAndOpToAndNot(binOp);
            if (replacementNode != nullptr)
            {
                return replacementNode->gtNext;
            }

            replacementNode = TryLowerAndOpToResetLowestSetBit(binOp);
            if (replacementNode != nullptr)
            {
                return replacementNode->gtNext;
            }

            replacementNode = TryLowerAndOpToExtractLowestSetBit(binOp);
            if (replacementNode != nullptr)
            {
                return replacementNode->gtNext;
            }
        }
        else if (binOp->OperIs(GT_XOR))
        {
            GenTree* replacementNode = TryLowerXorOpToGetMaskUpToLowestSetBit(binOp);
            if (replacementNode != nullptr)
            {
                return replacementNode->gtNext;
            }
        }
    }
#endif

    ContainCheckBinary(binOp);

#ifdef TARGET_AMD64
    if (JitConfig.EnableApxConditionalChaining())
    {
        if (binOp->OperIs(GT_AND, GT_OR))
        {
            GenTree* next;
            if (TryLowerAndOrToCCMP(binOp, &next))
            {
                return next;
            }
        }
    }
#endif // TARGET_AMD64

    return binOp->gtNext;
}

#ifdef TARGET_AMD64
//------------------------------------------------------------------------
// TruthifyingFlags: Get a flags immediate that will make a specified condition true.
//
// Arguments:
//    condition - the condition.
//
// Returns:
//    A flags immediate that, if those flags were set, would cause the specified condition to be true.
//    (NOTE: This just has to make the condition be true, i.e., if the condition calls for (SF ^ OF), then
//    returning one will suffice
insCflags Lowering::TruthifyingFlags(GenCondition condition)
{
    switch (condition.GetCode())
    {
        case GenCondition::EQ:
            return INS_FLAGS_ZF;
        case GenCondition::NE:
            return INS_FLAGS_NONE;
        case GenCondition::SGE: // !(SF ^ OF)
            return INS_FLAGS_NONE;
        case GenCondition::SGT: // !(SF ^ OF) && !ZF
            return INS_FLAGS_NONE;
        case GenCondition::SLE: // !(SF ^ OF) || ZF
            return INS_FLAGS_ZF;
        case GenCondition::SLT: // (SF ^ OF)
            return INS_FLAGS_SF;
        case GenCondition::UGE: // !CF
            return INS_FLAGS_NONE;
        case GenCondition::UGT: // !CF && !ZF
            return INS_FLAGS_NONE;
        case GenCondition::ULE: // CF || ZF
            return INS_FLAGS_ZF;
        case GenCondition::ULT: // CF
            return INS_FLAGS_CF;
        default:
            NO_WAY("unexpected condition type");
            return INS_FLAGS_NONE;
    }
}
#endif // TARGET_AMD64

//------------------------------------------------------------------------
// LowerBlockStore: Lower a block store node
//
// Arguments:
//    blkNode - The block store node to lower
//
void Lowering::LowerBlockStore(GenTreeBlk* blkNode)
{
    TryCreateAddrMode(blkNode->Addr(), false, blkNode);

    GenTree* dstAddr = blkNode->Addr();
    GenTree* src     = blkNode->Data();
    unsigned size    = blkNode->Size();

    if (blkNode->OperIsInitBlkOp())
    {
#ifdef DEBUG
        // Use BlkOpKindLoop for more cases under stress mode
        if (comp->compStressCompile(Compiler::STRESS_STORE_BLOCK_UNROLLING, 50) && blkNode->OperIs(GT_STORE_BLK) &&
            ((blkNode->GetLayout()->GetSize() % TARGET_POINTER_SIZE) == 0) && src->IsIntegralConst(0))
        {
            blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindLoop;
            return;
        }
#endif

        if (src->OperIs(GT_INIT_VAL))
        {
            src->SetContained();
            src = src->AsUnOp()->gtGetOp1();
        }

        if (size <= comp->getUnrollThreshold(Compiler::UnrollKind::Memset))
        {
            if (!src->OperIs(GT_CNS_INT))
            {
                // TODO-CQ: We could unroll even when the initialization value is not a constant
                // by inserting a MUL init, 0x01010101 instruction. We need to determine if the
                // extra latency that MUL introduces isn't worse that rep stosb. Likely not.
                blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindRepInstr;
            }
            else
            {
                // The fill value of an initblk is interpreted to hold a
                // value of (unsigned int8) however a constant of any size
                // may practically reside on the evaluation stack. So extract
                // the lower byte out of the initVal constant and replicate
                // it to a larger constant whose size is sufficient to support
                // the largest width store of the desired inline expansion.

                ssize_t fill = src->AsIntCon()->IconValue() & 0xFF;

                const bool canUseSimd = !blkNode->IsOnHeapAndContainsReferences();
                if (size > comp->getUnrollThreshold(Compiler::UnrollKind::Memset, canUseSimd))
                {
                    // It turns out we can't use SIMD so the default threshold is too big
                    goto TOO_BIG_TO_UNROLL;
                }
                if (canUseSimd && (size >= XMM_REGSIZE_BYTES))
                {
                    // We're going to use SIMD (and only SIMD - we don't want to occupy a GPR register
                    // with a fill value just to handle the remainder when we can do that with
                    // an overlapped SIMD load).
                    src->SetContained();
                }
                else if (fill == 0)
                {
                    // Leave as is - zero shouldn't be contained when we don't use SIMD.
                }
#ifdef TARGET_AMD64
                else if (size >= REGSIZE_BYTES)
                {
                    fill *= 0x0101010101010101LL;
                    src->gtType = TYP_LONG;
                }
#endif
                else
                {
                    fill *= 0x01010101;
                }

                blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindUnroll;
                src->AsIntCon()->SetIconValue(fill);

                ContainBlockStoreAddress(blkNode, size, dstAddr, nullptr);
            }
        }
        else
        {
        TOO_BIG_TO_UNROLL:
            if (blkNode->IsZeroingGcPointersOnHeap())
            {
                blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindLoop;
            }
            else
            {
#ifdef TARGET_AMD64
                LowerBlockStoreAsHelperCall(blkNode);
                return;
#else
                // TODO-X86-CQ: Investigate whether a helper call would be beneficial on x86
                blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindRepInstr;
#endif
            }
        }
    }
    else
    {
        assert(src->OperIs(GT_IND, GT_LCL_VAR, GT_LCL_FLD));
        src->SetContained();

        if (src->OperIs(GT_LCL_VAR))
        {
            // TODO-1stClassStructs: for now we can't work with STORE_BLOCK source in register.
            const unsigned srcLclNum = src->AsLclVar()->GetLclNum();
            comp->lvaSetVarDoNotEnregister(srcLclNum DEBUGARG(DoNotEnregisterReason::StoreBlkSrc));
        }

        ClassLayout* layout               = blkNode->GetLayout();
        bool         doCpObj              = layout->HasGCPtr();
        bool         isNotHeap            = blkNode->IsAddressNotOnHeap(comp);
        bool         canUseSimd           = !doCpObj || isNotHeap;
        unsigned     copyBlockUnrollLimit = comp->getUnrollThreshold(Compiler::UnrollKind::Memcpy, canUseSimd);

#ifndef JIT32_GCENCODER
        if (doCpObj && (size <= copyBlockUnrollLimit))
        {
            // No write barriers are needed if the destination is known to be outside of the GC heap.
            if (isNotHeap)
            {
                // If the size is small enough to unroll then we need to mark the block as non-interruptible
                // to actually allow unrolling. The generated code does not report GC references loaded in the
                // temporary register(s) used for copying. This is not supported for the JIT32_GCENCODER.
                doCpObj                  = false;
                blkNode->gtBlkOpGcUnsafe = true;
            }
        }
#endif

        if (doCpObj)
        {
            // Try to use bulk copy helper
            if (TryLowerBlockStoreAsGcBulkCopyCall(blkNode))
            {
                return;
            }

            assert(dstAddr->TypeIs(TYP_BYREF, TYP_I_IMPL));

            // If we have a long enough sequence of slots that do not require write barriers then
            // we can use REP MOVSD/Q instead of a sequence of MOVSD/Q instructions. According to the
            // Intel Manual, the sweet spot for small structs is between 4 to 12 slots of size where
            // the entire operation takes 20 cycles and encodes in 5 bytes (loading RCX and REP MOVSD/Q).
            unsigned nonGCSlots = 0;

            if (blkNode->IsAddressNotOnHeap(comp))
            {
                // If the destination is on the stack then no write barriers are needed.
                nonGCSlots = layout->GetSlotCount();
            }
            else
            {
                // Otherwise a write barrier is needed for every GC pointer in the layout
                // so we need to check if there's a long enough sequence of non-GC slots.
                unsigned slots = layout->GetSlotCount();
                for (unsigned i = 0; i < slots; i++)
                {
                    if (layout->IsGCPtr(i))
                    {
                        nonGCSlots = 0;
                    }
                    else
                    {
                        nonGCSlots++;

                        if (nonGCSlots >= CPOBJ_NONGC_SLOTS_LIMIT)
                        {
                            break;
                        }
                    }
                }
            }

            if (nonGCSlots >= CPOBJ_NONGC_SLOTS_LIMIT)
            {
                blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindCpObjRepInstr;
            }
            else
            {
                blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindCpObjUnroll;
            }
        }
        else if (blkNode->OperIs(GT_STORE_BLK) &&
                 (size <= comp->getUnrollThreshold(Compiler::UnrollKind::Memcpy, canUseSimd)))
        {
            blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindUnroll;

            if (src->OperIs(GT_IND))
            {
                ContainBlockStoreAddress(blkNode, size, src->AsIndir()->Addr(), src->AsIndir());
            }

            ContainBlockStoreAddress(blkNode, size, dstAddr, nullptr);
        }
        else
        {
            assert(blkNode->OperIs(GT_STORE_BLK));

#ifdef TARGET_AMD64
            LowerBlockStoreAsHelperCall(blkNode);
            return;
#else
            // TODO-X86-CQ: Investigate whether a helper call would be beneficial on x86
            blkNode->gtBlkOpKind = GenTreeBlk::BlkOpKindRepInstr;
#endif
        }
    }

    assert(blkNode->gtBlkOpKind != GenTreeBlk::BlkOpKindInvalid);
}

//------------------------------------------------------------------------
// ContainBlockStoreAddress: Attempt to contain an address used by an unrolled block store.
//
// Arguments:
//    blkNode - the block store node
//    size - the block size
//    addr - the address node to try to contain
//    addrParent - the parent of addr, in case this is checking containment of the source address.
//
void Lowering::ContainBlockStoreAddress(GenTreeBlk* blkNode, unsigned size, GenTree* addr, GenTree* addrParent)
{
    assert(blkNode->OperIs(GT_STORE_BLK) && (blkNode->gtBlkOpKind == GenTreeBlk::BlkOpKindUnroll));
    assert(size < INT32_MAX);

    if (addr->OperIs(GT_LCL_ADDR) && IsContainableLclAddr(addr->AsLclFld(), size))
    {
        addr->SetContained();
        return;
    }

    if (!addr->OperIsAddrMode() && !TryCreateAddrMode(addr, true, blkNode))
    {
        return;
    }

    GenTreeAddrMode* addrMode = addr->AsAddrMode();

    // On x64 the address mode displacement is signed so it must not exceed INT32_MAX. This check is
    // an approximation since the last displacement we generate in an unrolled block operation can be
    // up to 16 bytes lower than offset + size. But offsets large enough to hit this case are likely
    // to be extremely rare for this to ever be a CQ issue.
    // On x86 this shouldn't be needed but then again, offsets large enough to hit this are rare.
    if (addrMode->Offset() > (INT32_MAX - static_cast<int>(size)))
    {
        return;
    }

    // Note that the parentNode is always the block node, even if we're dealing with the source address.
    // The source address is not directly used by the block node but by an IND node and that IND node is
    // always contained.
    if (!IsInvariantInRange(addrMode, blkNode, addrParent))
    {
        return;
    }

    addrMode->SetContained();
}

//------------------------------------------------------------------------
// LowerPutArgStk: Lower a GT_PUTARG_STK.
//
// Arguments:
//    putArgStk - The node of interest
//
void Lowering::LowerPutArgStk(GenTreePutArgStk* putArgStk)
{
    GenTree* src        = putArgStk->Data();
    bool     srcIsLocal = src->OperIsLocalRead();

    if (src->OperIs(GT_FIELD_LIST))
    {
#ifdef TARGET_X86
        GenTreeFieldList* fieldList = src->AsFieldList();

        // The code generator will push these fields in reverse order by offset. Reorder the list here s.t. the order
        // of uses is visible to LSRA.
        assert(fieldList->Uses().IsSorted());
        fieldList->Uses().Reverse();

        // Containment checks.
        for (GenTreeFieldList::Use& use : fieldList->Uses())
        {
            GenTree* const  fieldNode = use.GetNode();
            const var_types fieldType = use.GetType();
            assert(!fieldNode->TypeIs(TYP_LONG));

            // For x86 we must mark all integral fields as contained or reg-optional, and handle them
            // accordingly in code generation, since we may have up to 8 fields, which cannot all be in
            // registers to be consumed atomically by the call.
            if (varTypeIsIntegralOrI(fieldNode))
            {
                if (IsContainableImmed(putArgStk, fieldNode))
                {
                    MakeSrcContained(putArgStk, fieldNode);
                }
                else if (IsContainableMemoryOp(fieldNode) && IsSafeToContainMem(putArgStk, fieldNode))
                {
                    MakeSrcContained(putArgStk, fieldNode);
                }
                else
                {
                    // For the case where we cannot directly push the value, if we run out of registers,
                    // it would be better to defer computation until we are pushing the arguments rather
                    // than spilling, but this situation is not all that common, as most cases of FIELD_LIST
                    // are promoted structs, which do not not have a large number of fields, and of those
                    // most are lclVars or copy-propagated constants.

                    fieldNode->SetRegOptional();
                }
            }
        }

        // Set the copy kind.
        // TODO-X86-CQ: Even if we are using push, if there are contiguous floating point fields, we should
        // adjust the stack once for those fields. The latter is really best done in code generation, but
        // this tuning should probably be undertaken as a whole.
        // Also, if there are  floating point fields, it may be better to use the "Unroll" mode
        // of copying the struct as a whole, if the fields are not register candidates.
        putArgStk->gtPutArgStkKind = GenTreePutArgStk::Kind::Push;
#endif // TARGET_X86
        return;
    }

    if (src->TypeIs(TYP_STRUCT))
    {
        assert(src->OperIs(GT_BLK) || src->OperIsLocalRead());

        ClassLayout* layout  = src->GetLayout(comp);
        var_types    regType = layout->GetRegisterType();

        if (regType == TYP_UNDEF)
        {
            // In case of a CpBlk we could use a helper call. In case of putarg_stk we
            // can't do that since the helper call could kill some already set up outgoing args.
            // TODO-Amd64-Unix: converge the code for putarg_stk with cpyblk/cpyobj.
            // The cpyXXXX code is rather complex and this could cause it to be more complex, but
            // it might be the right thing to do.

            // If possible, widen the load, this results in more compact code.
            unsigned loadSize = srcIsLocal ? roundUp(layout->GetSize(), TARGET_POINTER_SIZE) : layout->GetSize();
            putArgStk->SetArgLoadSize(loadSize);

            // TODO-X86-CQ: The helper call either is not supported on x86 or required more work
            // (I don't know which).
            if (!layout->HasGCPtr())
            {
#ifdef TARGET_X86
                // Codegen for "Kind::Push" will always load bytes in TARGET_POINTER_SIZE
                // chunks. As such, we'll only use this path for correctly-sized sources.
                if ((loadSize < XMM_REGSIZE_BYTES) && ((loadSize % TARGET_POINTER_SIZE) == 0))
                {
                    putArgStk->gtPutArgStkKind = GenTreePutArgStk::Kind::Push;
                }
                else
#endif // TARGET_X86
                    if (loadSize <= comp->getUnrollThreshold(Compiler::UnrollKind::Memcpy))
                    {
                        putArgStk->gtPutArgStkKind = GenTreePutArgStk::Kind::Unroll;
                    }
                    else
                    {
                        putArgStk->gtPutArgStkKind = GenTreePutArgStk::Kind::RepInstr;
                    }
            }
            else // There are GC pointers.
            {
#ifdef TARGET_X86
                // On x86, we must use `push` to store GC references to the stack in order for the emitter to
                // properly update the function's GC info. These `putargstk` nodes will generate a sequence of
                // `push` instructions.
                putArgStk->gtPutArgStkKind = GenTreePutArgStk::Kind::Push;
#else  // !TARGET_X86
                putArgStk->gtPutArgStkKind = GenTreePutArgStk::Kind::PartialRepInstr;
#endif // !TARGET_X86
            }

            if (src->OperIs(GT_LCL_VAR))
            {
                comp->lvaSetVarDoNotEnregister(src->AsLclVar()->GetLclNum()
                                                   DEBUGARG(DoNotEnregisterReason::IsStructArg));
            }

            // Always mark the OBJ/LCL_VAR/LCL_FLD as contained trees.
            MakeSrcContained(putArgStk, src);
        }
        else
        {
            // The ABI allows upper bits of small struct args to remain undefined,
            // so if possible, widen the load to avoid the sign/zero-extension.
            if (varTypeIsSmall(regType) && srcIsLocal)
            {
                assert(genTypeSize(TYP_INT) <= putArgStk->GetStackByteSize());
                regType = TYP_INT;
            }

            src->ChangeType(regType);

            if (src->OperIs(GT_BLK))
            {
                src->SetOper(GT_IND);
                LowerIndir(src->AsIndir());
            }
        }
    }

    if (src->TypeIs(TYP_STRUCT))
    {
        return;
    }

    assert(!src->TypeIs(TYP_STRUCT));

    // If the child of GT_PUTARG_STK is a constant, we don't need a register to
    // move it to memory (stack location).
    //
    // On AMD64, we don't want to make 0 contained, because we can generate smaller code
    // by zeroing a register and then storing it. E.g.:
    //      xor rdx, rdx
    //      mov gword ptr [rsp+28H], rdx
    // is 2 bytes smaller than:
    //      mov gword ptr [rsp+28H], 0
    //
    // On x86, we push stack arguments; we don't use 'mov'. So:
    //      push 0
    // is 1 byte smaller than:
    //      xor rdx, rdx
    //      push rdx

    if (IsContainableImmed(putArgStk, src)
#if defined(TARGET_AMD64)
        && !src->IsIntegralConst(0)
#endif // TARGET_AMD64
    )
    {
        MakeSrcContained(putArgStk, src);
    }
#ifdef TARGET_X86
    else if (genTypeSize(src) == TARGET_POINTER_SIZE)
    {
        // We can use "src" directly from memory with "push [mem]".
        TryMakeSrcContainedOrRegOptional(putArgStk, src);
    }
#endif // TARGET_X86
}

//------------------------------------------------------------------------
// LowerCast: Lower GT_CAST(srcType, DstType) nodes.
//
// Arguments:
//    tree - GT_CAST node to be lowered
//
// Return Value:
//    None.
//
void Lowering::LowerCast(GenTree* tree)
{
    assert(tree->OperIs(GT_CAST));

    GenTree*  castOp  = tree->AsCast()->CastOp();
    var_types dstType = tree->CastToType();
    var_types srcType = castOp->TypeGet();

    // force the srcType to unsigned if GT_UNSIGNED flag is set
    if (tree->IsUnsigned())
    {
        srcType = varTypeToUnsigned(srcType);
    }

    if (varTypeIsFloating(srcType))
    {
        // Overflow casts should have been converted to helper call in morph.
        noway_assert(!tree->gtOverflow());
        // Small types should have had an intermediate int cast inserted in morph.
        assert(!varTypeIsSmall(dstType));
        // Long types should have been handled by helper call or in DecomposeLongs on x86.
        assert(!varTypeIsLong(dstType) || TargetArchitecture::Is64Bit);
    }
    else if (srcType == TYP_UINT)
    {
        // uint->float casts should have an intermediate cast to long unless
        // we have the EVEX unsigned conversion instructions available.
        assert(dstType != TYP_FLOAT || comp->canUseEvexEncodingDebugOnly());
    }

#ifdef FEATURE_HW_INTRINSICS
    if (varTypeIsFloating(srcType) && varTypeIsIntegral(dstType) &&
        !comp->compOpportunisticallyDependsOn(InstructionSet_AVX10v2))
    {
        // If we don't have AVX10v2 saturating conversion instructions for
        // floating->integral, we have to handle the saturation logic here.

        JITDUMP("LowerCast before:\n");
        DISPTREERANGE(BlockRange(), tree);

        CorInfoType srcBaseType = (srcType == TYP_FLOAT) ? CORINFO_TYPE_FLOAT : CORINFO_TYPE_DOUBLE;
        LIR::Range  castRange   = LIR::EmptyRange();

        // We'll be using SIMD instructions to fix up castOp before conversion.
        //
        // This creates the equivalent of the following C# code:
        //   var srcVec = Vector128.CreateScalarUnsafe(castOp);

        GenTree* srcVector = comp->gtNewSimdCreateScalarUnsafeNode(TYP_SIMD16, castOp, srcBaseType, 16);
        castRange.InsertAtEnd(srcVector);

        if (srcVector->IsCnsVec())
        {
            castOp->SetUnusedValue();
        }

        if (varTypeIsUnsigned(dstType) && comp->canUseEvexEncoding())
        {
            // EVEX unsigned conversion instructions saturate positive overflow properly, so as
            // long as we fix up NaN and negative values, we can preserve the existing cast node.
            //
            // maxs[sd] will take the value from the second operand if the first operand's value is
            // NaN, which allows us to fix up both negative and NaN values with a single instruction.
            //
            // This creates the equivalent of the following C# code:
            //   castOp = Sse.MaxScalar(srcVec, Vector128<T>.Zero).ToScalar();

            NamedIntrinsic maxScalarIntrinsic = NI_X86Base_MaxScalar;

            GenTree* zero = comp->gtNewZeroConNode(TYP_SIMD16);
            GenTree* fixupVal =
                comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, srcVector, zero, maxScalarIntrinsic, srcBaseType, 16);

            GenTree* toScalar = comp->gtNewSimdToScalarNode(srcType, fixupVal, srcBaseType, 16);

            castRange.InsertAtEnd(zero);
            castRange.InsertAtEnd(fixupVal);
            castRange.InsertAtEnd(toScalar);

            tree->AsCast()->CastOp() = toScalar;
        }
        else
        {
            // We need to fix up NaN as well as handle possible overflow. Signed conversions
            // return int/long.MinValue for any overflow, which is correct for saturation of
            // negative, but the result must be replaced with MaxValue for positive overflow.

            CorInfoType    dstBaseType      = CORINFO_TYPE_UNDEF;
            NamedIntrinsic convertIntrinsic = NI_Illegal;
            GenTree*       maxIntegralValue = nullptr;
            GenTree*       maxFloatingValue = comp->gtNewVconNode(TYP_SIMD16);
            simd_t*        maxFloatSimdVal  = &maxFloatingValue->AsVecCon()->gtSimdVal;

            switch (dstType)
            {
                case TYP_INT:
                {
                    dstBaseType      = CORINFO_TYPE_INT;
                    maxIntegralValue = comp->gtNewIconNode(INT32_MAX);
                    if (srcType == TYP_FLOAT)
                    {
                        maxFloatSimdVal->f32[0] = 2147483648.0f;
                        convertIntrinsic        = NI_X86Base_ConvertToInt32WithTruncation;
                    }
                    else
                    {
                        maxFloatSimdVal->f64[0] = 2147483648.0;
                        convertIntrinsic        = NI_X86Base_ConvertToInt32WithTruncation;
                    }
                    break;
                }
                case TYP_UINT:
                {
                    dstBaseType      = CORINFO_TYPE_UINT;
                    maxIntegralValue = comp->gtNewIconNode(static_cast<ssize_t>(UINT32_MAX));
                    if (srcType == TYP_FLOAT)
                    {
                        maxFloatSimdVal->f32[0] = 4294967296.0f;
                        convertIntrinsic        = TargetArchitecture::Is64Bit
                                                      ? NI_X86Base_X64_ConvertToInt64WithTruncation
                                                      : NI_X86Base_ConvertToVector128Int32WithTruncation;
                    }
                    else
                    {
                        maxFloatSimdVal->f64[0] = 4294967296.0;
                        convertIntrinsic        = TargetArchitecture::Is64Bit
                                                      ? NI_X86Base_X64_ConvertToInt64WithTruncation
                                                      : NI_X86Base_ConvertToVector128Int32WithTruncation;
                    }
                    break;
                }
                case TYP_LONG:
                {
                    dstBaseType      = CORINFO_TYPE_LONG;
                    maxIntegralValue = comp->gtNewLconNode(INT64_MAX);
                    if (srcType == TYP_FLOAT)
                    {
                        maxFloatSimdVal->f32[0] = 9223372036854775808.0f;
                        convertIntrinsic        = NI_X86Base_X64_ConvertToInt64WithTruncation;
                    }
                    else
                    {
                        maxFloatSimdVal->f64[0] = 9223372036854775808.0;
                        convertIntrinsic        = NI_X86Base_X64_ConvertToInt64WithTruncation;
                    }
                    break;
                }
                case TYP_ULONG:
                {
                    dstBaseType      = CORINFO_TYPE_ULONG;
                    maxIntegralValue = comp->gtNewLconNode(static_cast<int64_t>(UINT64_MAX));
                    if (srcType == TYP_FLOAT)
                    {
                        maxFloatSimdVal->f32[0] = 18446744073709551616.0f;
                        convertIntrinsic        = NI_X86Base_X64_ConvertToInt64WithTruncation;
                    }
                    else
                    {
                        maxFloatSimdVal->f64[0] = 18446744073709551616.0;
                        convertIntrinsic        = NI_X86Base_X64_ConvertToInt64WithTruncation;
                    }
                    break;
                }
                default:
                {
                    unreached();
                }
            }

            // We will use the input value at least twice, so we preemptively replace it with a lclVar.
            LIR::Use srcUse;
            LIR::Use::MakeDummyUse(castRange, srcVector, &srcUse);
            srcUse.ReplaceWithLclVar(comp);
            srcVector = srcUse.Def();

            GenTree* srcClone      = nullptr;
            GenTree* convertResult = nullptr;

            if (varTypeIsSigned(dstType))
            {
                // Fix up NaN values before conversion. Saturation is handled after conversion,
                // because MaxValue may not be precisely representable in the floating format.
                //
                // This creates the equivalent of the following C# code:
                //   var nanMask = Sse.CompareScalarOrdered(srcVec, srcVec);
                //   var fixupVal = Sse.And(srcVec, nanMask);
                //   convertResult = Sse.ConvertToInt32WithTruncation(fixupVal);

                NamedIntrinsic compareNaNIntrinsic = NI_X86Base_CompareScalarOrdered;

                srcClone         = comp->gtClone(srcVector);
                GenTree* nanMask = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, srcVector, srcClone, compareNaNIntrinsic,
                                                                  srcBaseType, 16);

                castRange.InsertAtEnd(srcClone);
                castRange.InsertAtEnd(nanMask);

                srcClone          = comp->gtClone(srcVector);
                GenTree* fixupVal = comp->gtNewSimdBinOpNode(GT_AND, TYP_SIMD16, nanMask, srcClone, srcBaseType, 16);

                castRange.InsertAtEnd(srcClone);
                castRange.InsertAtEnd(fixupVal);

                convertResult = comp->gtNewSimdHWIntrinsicNode(dstType, fixupVal, convertIntrinsic, srcBaseType, 16);
            }
            else
            {
                // maxs[sd] will take the value from the second operand if the first operand's value is
                // NaN, which allows us to fix up both negative and NaN values with a single instruction.
                //
                // This creates the equivalent of the following C# code:
                //   var fixupVal = Sse.MaxScalar(srcVec, Vector128<T>.Zero);

                NamedIntrinsic maxScalarIntrinsic = NI_X86Base_MaxScalar;

                GenTree* zero = comp->gtNewZeroConNode(TYP_SIMD16);
                GenTree* fixupVal =
                    comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, srcVector, zero, maxScalarIntrinsic, srcBaseType, 16);

                castRange.InsertAtEnd(zero);
                castRange.InsertAtEnd(fixupVal);

                if ((dstType == TYP_UINT) && (convertIntrinsic == NI_X86Base_X64_ConvertToInt64WithTruncation))
                {
                    // On x64, we can use long conversion to handle uint directly.
                    convertResult =
                        comp->gtNewSimdHWIntrinsicNode(TYP_LONG, fixupVal, convertIntrinsic, srcBaseType, 16);
                }
                else
                {
                    // We're doing a conversion that isn't supported directly by hardware. We will emulate
                    // the unsigned conversion by using the signed instruction on both the fixed-up input
                    // value and a negative value that has the same bit representation when converted to
                    // integer. If the conversion overflows as a signed integer, the negative conversion
                    // result is selected.
                    //
                    // This creates the equivalent of the following C# code:
                    //   var wrapVal = Sse.SubtractScalar(srcVec, maxFloatingValue);

                    NamedIntrinsic subtractIntrinsic = NI_X86Base_SubtractScalar;

                    // We're going to use maxFloatingValue twice, so replace the constant with a lclVar.
                    castRange.InsertAtEnd(maxFloatingValue);

                    LIR::Use maxFloatUse;
                    LIR::Use::MakeDummyUse(castRange, maxFloatingValue, &maxFloatUse);
                    maxFloatUse.ReplaceWithLclVar(comp);
                    maxFloatingValue = maxFloatUse.Def();

                    GenTree* floorVal = comp->gtClone(srcVector);
                    castRange.InsertAtEnd(floorVal);

                    if ((srcType == TYP_DOUBLE) && (dstType == TYP_UINT))
                    {
                        // This technique works only if the truncating conversion of the positive and negative
                        // values causes them to round in the same direction. i.e. there is no rounding, because
                        // we have a whole number. This is always true if the exponent is larger than the number
                        // of significand bits, which will always be the case for double->ulong or float->uint.
                        //
                        // For double->uint, the double has enough precision to exactly represent any whole number
                        // in range, with bits left over. e.g. we might have a value of 4294967295.9999995.
                        // We must, therefore, truncate the value before wrapping it to negative.

                        if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                        {
                            // This creates the equivalent of the following C# code:
                            //   floorVal = Sse41.RoundToZeroScalar(srcVector);

                            floorVal = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, floorVal, NI_SSE42_RoundToZeroScalar,
                                                                      srcBaseType, 16);
                            castRange.InsertAtEnd(floorVal);
                        }
                        else
                        {
                            // We don't have `roundsd` available, but we can truncate the value by simply zeroing out
                            // the low 21 bits of the double. This works because we know we will only use the negative
                            // value when the exponent is exactly 31, meaning 31 of the 52 bits in the significand are
                            // used for the whole portion of the number, and the remaining 21 bits are fractional.
                            //
                            // This creates the equivalent of the following C# code:
                            //   floorVal = ((srcVector.AsUInt64() >>> 21) << 21).AsDouble();

                            GenTree* twentyOne  = comp->gtNewIconNode(21);
                            GenTree* rightShift = comp->gtNewSimdBinOpNode(GT_RSZ, TYP_SIMD16, floorVal, twentyOne,
                                                                           CORINFO_TYPE_ULONG, 16);
                            castRange.InsertAtEnd(twentyOne);
                            castRange.InsertAtEnd(rightShift);

                            twentyOne = comp->gtClone(twentyOne);
                            floorVal  = comp->gtNewSimdBinOpNode(GT_LSH, TYP_SIMD16, rightShift, twentyOne,
                                                                 CORINFO_TYPE_ULONG, 16);
                            castRange.InsertAtEnd(twentyOne);
                            castRange.InsertAtEnd(floorVal);
                        }
                    }

                    GenTree* wrapVal = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, floorVal, maxFloatingValue,
                                                                      subtractIntrinsic, srcBaseType, 16);
                    castRange.InsertAtEnd(wrapVal);

                    maxFloatingValue = comp->gtClone(maxFloatingValue);

                    if (dstType == TYP_UINT)
                    {
                        // We can keep the conversion results in SIMD registers to make selection of the
                        // correct result simpler.
                        //
                        // This creates the equivalent of the following C# code:
                        //   var result = Sse2.ConvertToVector128Int32WithTruncation(fixupVal);
                        //   var negated = Sse2.ConvertToVector128Int32WithTruncation(wrapVal);

                        GenTree* result =
                            comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, fixupVal, convertIntrinsic, srcBaseType, 16);
                        GenTree* negated =
                            comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, wrapVal, convertIntrinsic, srcBaseType, 16);

                        castRange.InsertAtEnd(result);
                        castRange.InsertAtEnd(negated);

                        // We need the result twice -- one for the mask bit and one for the blend.
                        LIR::Use resultUse;
                        LIR::Use::MakeDummyUse(castRange, result, &resultUse);
                        resultUse.ReplaceWithLclVar(comp);
                        result = resultUse.Def();

                        GenTree* resultClone = comp->gtClone(result);
                        castRange.InsertAtEnd(resultClone);

                        if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                        {
                            // If the conversion of the fixed-up value overflowed, the result wil be
                            // int.MinValue. Since `blendvps` uses only the MSB for result selection,
                            // this is adequate to force selection of the negated result.
                            //
                            // This creates the equivalent of the following C# code:
                            //   convertResult = Sse41.BlendVariable(result, negated, result);

                            convertResult =
                                comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, result, negated, resultClone,
                                                               NI_SSE42_BlendVariable, CORINFO_TYPE_FLOAT, 16);
                        }
                        else
                        {
                            // If we can't use `blendvps`, we do a bit-wise selection. This works
                            // using only and+or because if we choose the negated value, both it
                            // and the overflowed result have MSB set.
                            //
                            // This creates the equivalent of the following C# code:
                            //   var mask = Sse2.ShiftRightArithmetic(result, 31);
                            //   convertResult = Sse.Or(result, Sse.And(negated, mask));

                            GenTree* thirtyOne = comp->gtNewIconNode(31);
                            GenTree* mask =
                                comp->gtNewSimdBinOpNode(GT_RSH, TYP_SIMD16, result, thirtyOne, CORINFO_TYPE_INT, 16);
                            GenTree* andMask =
                                comp->gtNewSimdBinOpNode(GT_AND, TYP_SIMD16, mask, negated, dstBaseType, 16);

                            castRange.InsertAtEnd(thirtyOne);
                            castRange.InsertAtEnd(mask);
                            castRange.InsertAtEnd(andMask);

                            convertResult =
                                comp->gtNewSimdBinOpNode(GT_OR, TYP_SIMD16, andMask, resultClone, dstBaseType, 16);
                        }

                        // Because the results are in a SIMD register, we need to ToScalar() them out.
                        castRange.InsertAtEnd(convertResult);
                        convertResult = comp->gtNewSimdToScalarNode(TYP_INT, convertResult, dstBaseType, 16);
                    }
                    else
                    {
                        assert(dstType == TYP_ULONG);

                        // We're emulating floating->ulong conversion on x64. The logic is the same as for
                        // uint on x86, except that we don't have conversion instructions that keep the
                        // results in SIMD registers, so we do the final result selection in scalar code.
                        //
                        // This creates the equivalent of the following C# code:
                        //   long result = Sse.X64.ConvertToInt64WithTruncation(fixupVal);
                        //   long negated = Sse.X64.ConvertToInt64WithTruncation(wrapVal);
                        //   convertResult = (ulong)(result | (negated & (result >> 63)));

                        GenTree* result =
                            comp->gtNewSimdHWIntrinsicNode(TYP_LONG, fixupVal, convertIntrinsic, srcBaseType, 16);
                        GenTree* negated =
                            comp->gtNewSimdHWIntrinsicNode(TYP_LONG, wrapVal, convertIntrinsic, srcBaseType, 16);

                        castRange.InsertAtEnd(result);
                        castRange.InsertAtEnd(negated);

                        // We need the result twice -- once for the mask bit and once for the blend.
                        LIR::Use resultUse;
                        LIR::Use::MakeDummyUse(castRange, result, &resultUse);
                        resultUse.ReplaceWithLclVar(comp);
                        result = resultUse.Def();

                        GenTree* sixtyThree  = comp->gtNewIconNode(63);
                        GenTree* mask        = comp->gtNewOperNode(GT_RSH, TYP_LONG, result, sixtyThree);
                        GenTree* andMask     = comp->gtNewOperNode(GT_AND, TYP_LONG, mask, negated);
                        GenTree* resultClone = comp->gtClone(result);

                        castRange.InsertAtEnd(sixtyThree);
                        castRange.InsertAtEnd(mask);
                        castRange.InsertAtEnd(andMask);
                        castRange.InsertAtEnd(resultClone);

                        convertResult = comp->gtNewOperNode(GT_OR, TYP_LONG, andMask, resultClone);
                    }
                }
            }

            // Now we handle saturation of the result for positive overflow.
            //
            // This creates the equivalent of the following C# code:
            //   bool isOverflow = Sse.CompareScalarUnorderedGreaterThanOrEqual(srcVec, maxFloatingValue);
            //   return isOverflow ? maxIntegralValue : convertResult;

            NamedIntrinsic compareIntrinsic = NI_X86Base_CompareScalarUnorderedGreaterThanOrEqual;

            // These nodes were all created above but not used until now.
            castRange.InsertAtEnd(maxFloatingValue);
            castRange.InsertAtEnd(maxIntegralValue);
            castRange.InsertAtEnd(convertResult);

            srcClone            = comp->gtClone(srcVector);
            GenTree* compareMax = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, srcClone, maxFloatingValue,
                                                                 compareIntrinsic, srcBaseType, 16);
            GenTree* select     = comp->gtNewConditionalNode(GT_SELECT, compareMax, maxIntegralValue, convertResult,
                                                             genActualType(dstType));

            castRange.InsertAtEnd(srcClone);
            castRange.InsertAtEnd(compareMax);
            castRange.InsertAtEnd(select);

            // The original cast becomes a no-op, because its input is already the correct type.
            tree->AsCast()->CastOp() = select;
        }

        LIR::ReadOnlyRange lowerRange(castRange.FirstNode(), castRange.LastNode());
        BlockRange().InsertBefore(tree, std::move(castRange));

        JITDUMP("LowerCast after:\n");
        DISPTREERANGE(BlockRange(), tree);

        LowerRange(lowerRange);
    }
#endif // FEATURE_HW_INTRINSICS

    // Now determine if we have operands that should be contained.
    ContainCheckCast(tree->AsCast());
}

#ifdef FEATURE_HW_INTRINSICS

//----------------------------------------------------------------------------------------------
// LowerHWIntrinsicCC: Lowers a hardware intrinsic node that produces a boolean value by
//     setting the condition flags.
//
//  Arguments:
//     node - The hardware intrinsic node
//     newIntrinsicId - The intrinsic id of the lowered intrinsic node
//     condition - The condition code of the generated SETCC/JCC node
//
void Lowering::LowerHWIntrinsicCC(GenTreeHWIntrinsic* node, NamedIntrinsic newIntrinsicId, GenCondition condition)
{
    GenTreeCC* cc = LowerNodeCC(node, condition);

    assert((HWIntrinsicInfo::lookupNumArgs(newIntrinsicId) == 2) || (newIntrinsicId == NI_AVX512_KORTEST));
    node->ChangeHWIntrinsicId(newIntrinsicId);
    node->gtType = TYP_VOID;
    node->ClearUnusedValue();

    bool swapOperands    = false;
    bool canSwapOperands = false;

    switch (newIntrinsicId)
    {
        case NI_X86Base_COMIS:
        case NI_X86Base_UCOMIS:
            // In some cases we can generate better code if we swap the operands:
            //   - If the condition is not one of the "preferred" floating point conditions we can swap
            //     the operands and change the condition to avoid generating an extra JP/JNP branch.
            //   - If the first operand can be contained but the second cannot, we can swap operands in
            //     order to be able to contain the first operand and avoid the need for a temp reg.
            // We can't handle both situations at the same time and since an extra branch is likely to
            // be worse than an extra temp reg (x64 has a reasonable number of XMM registers) we'll favor
            // the branch case:
            //   - If the condition is not preferred then swap, even if doing this will later prevent
            //     containment.
            //   - Allow swapping for containment purposes only if this doesn't result in a non-"preferred"
            //     condition being generated.
            if ((cc != nullptr) && cc->gtCondition.PreferSwap())
            {
                swapOperands = true;
            }
            else
            {
                canSwapOperands = (cc == nullptr) || !GenCondition::Swap(cc->gtCondition).PreferSwap();
            }
            break;

        case NI_SSE42_PTEST:
        case NI_AVX_PTEST:
        {
            // If we need the Carry flag then we can't swap operands.
            canSwapOperands = (cc == nullptr) || cc->gtCondition.Is(GenCondition::EQ, GenCondition::NE);
            break;
        }

        case NI_AVX512_KORTEST:
        case NI_AVX512_KTEST:
        {
            // No containment support, so no reason to swap operands
            canSwapOperands = false;
            break;
        }

        default:
            unreached();
    }

    if (canSwapOperands)
    {
        bool op1SupportsRegOptional = false;
        bool op2SupportsRegOptional = false;

        if (!IsContainableHWIntrinsicOp(node, node->Op(2), &op2SupportsRegOptional) &&
            IsContainableHWIntrinsicOp(node, node->Op(1), &op1SupportsRegOptional))
        {
            // Swap operands if op2 cannot be contained but op1 can.
            swapOperands = true;
        }
    }

    if (swapOperands)
    {
        std::swap(node->Op(1), node->Op(2));

        if (cc != nullptr)
        {
            cc->gtCondition = GenCondition::Swap(cc->gtCondition);
        }
    }
}

//----------------------------------------------------------------------------------------------
// LowerFusedMultiplyOp: Changes FusedMultiply* operation produced
//     to a better FMA intrinsics if there are GT_NEG around in order
//     to eliminate them.
//
//  Arguments:
//     node - The hardware intrinsic node
//
//  Notes:
//      x *  y + z -> FusedMultiplyAdd
//      x * -y + z -> FusedMultiplyAddNegated
//     -x *  y + z -> FusedMultiplyAddNegated
//     -x * -y + z -> FusedMultiplyAdd
//      x *  y - z -> FusedMultiplySubtract
//      x * -y - z -> FusedMultiplySubtractNegated
//     -x *  y - z -> FusedMultiplySubtractNegated
//     -x * -y - z -> FusedMultiplySubtract
//
void Lowering::LowerFusedMultiplyOp(GenTreeHWIntrinsic* node)
{
    assert(node->GetOperandCount() == 3);

    bool negated  = false;
    bool subtract = false;
    bool isScalar = false;

    NamedIntrinsic intrinsic = node->GetHWIntrinsicId();

    switch (intrinsic)
    {
        case NI_AVX2_MultiplyAdd:
        case NI_AVX512_FusedMultiplyAdd:
        {
            break;
        }

        case NI_AVX2_MultiplyAddScalar:
        case NI_AVX512_FusedMultiplyAddScalar:
        {
            isScalar = true;
            break;
        }

        case NI_AVX2_MultiplyAddNegated:
        case NI_AVX512_FusedMultiplyAddNegated:
        {
            negated = true;
            break;
        }

        case NI_AVX2_MultiplyAddNegatedScalar:
        case NI_AVX512_FusedMultiplyAddNegatedScalar:
        {
            negated  = true;
            isScalar = true;
            break;
        }

        case NI_AVX2_MultiplySubtract:
        case NI_AVX512_FusedMultiplySubtract:
        {
            subtract = true;
            break;
        }

        case NI_AVX2_MultiplySubtractScalar:
        case NI_AVX512_FusedMultiplySubtractScalar:
        {
            subtract = true;
            isScalar = true;
            break;
        }

        case NI_AVX2_MultiplySubtractNegated:
        case NI_AVX512_FusedMultiplySubtractNegated:
        {
            subtract = true;
            negated  = true;
            break;
        }

        case NI_AVX2_MultiplySubtractNegatedScalar:
        case NI_AVX512_FusedMultiplySubtractNegatedScalar:
        {
            subtract = true;
            negated  = true;
            isScalar = true;
            break;
        }

        default:
        {
            unreached();
        }
    }

    bool negatedArgs[3] = {};

    for (size_t i = 1; i <= 3; i++)
    {
        GenTree* arg = node->Op(i);

        if (!arg->OperIsHWIntrinsic())
        {
            continue;
        }

        GenTreeHWIntrinsic* hwArg = arg->AsHWIntrinsic();

        switch (hwArg->GetHWIntrinsicId())
        {
            case NI_Vector128_CreateScalarUnsafe:
            case NI_Vector256_CreateScalarUnsafe:
            case NI_Vector512_CreateScalarUnsafe:
            {
                GenTree*& argOp = hwArg->Op(1);

                if (argOp->OperIs(GT_NEG))
                {
                    BlockRange().Remove(argOp);

                    argOp = argOp->gtGetOp1();
                    argOp->ClearContained();
                    ContainCheckHWIntrinsic(arg->AsHWIntrinsic());

                    negatedArgs[i - 1] ^= true;
                }

                break;
            }

            default:
            {
                bool       isScalarArg = false;
                genTreeOps oper        = hwArg->GetOperForHWIntrinsicId(&isScalarArg);

                if (oper != GT_XOR)
                {
                    break;
                }

                GenTree* argOp = hwArg->Op(2);

                if (!argOp->isContained())
                {
                    // A constant should have already been contained
                    break;
                }

                // xor is bitwise and the actual xor node might be a different base type
                // from the FMA node, so we check if its negative zero using the FMA base
                // type since that's what the end negation would end up using

                if (argOp->IsVectorNegativeZero(node->GetSimdBaseType()))
                {
                    BlockRange().Remove(hwArg);
                    BlockRange().Remove(argOp);

                    argOp = hwArg->Op(1);
                    argOp->ClearContained();
                    node->Op(i) = argOp;

                    negatedArgs[i - 1] ^= true;
                }

                break;
            }
        }
    }

    negated ^= negatedArgs[0];
    negated ^= negatedArgs[1];
    subtract ^= negatedArgs[2];

    if (intrinsic >= FIRST_NI_AVX512)
    {
        if (negated)
        {
            if (subtract)
            {
                intrinsic =
                    isScalar ? NI_AVX512_FusedMultiplySubtractNegatedScalar : NI_AVX512_FusedMultiplySubtractNegated;
            }
            else
            {
                intrinsic = isScalar ? NI_AVX512_FusedMultiplyAddNegatedScalar : NI_AVX512_FusedMultiplyAddNegated;
            }
        }
        else if (subtract)
        {
            intrinsic = isScalar ? NI_AVX512_FusedMultiplySubtractScalar : NI_AVX512_FusedMultiplySubtract;
        }
        else
        {
            intrinsic = isScalar ? NI_AVX512_FusedMultiplyAddScalar : NI_AVX512_FusedMultiplyAdd;
        }
    }
    else if (negated)
    {
        if (subtract)
        {
            intrinsic = isScalar ? NI_AVX2_MultiplySubtractNegatedScalar : NI_AVX2_MultiplySubtractNegated;
        }
        else
        {
            intrinsic = isScalar ? NI_AVX2_MultiplyAddNegatedScalar : NI_AVX2_MultiplyAddNegated;
        }
    }
    else if (subtract)
    {
        intrinsic = isScalar ? NI_AVX2_MultiplySubtractScalar : NI_AVX2_MultiplySubtract;
    }
    else
    {
        intrinsic = isScalar ? NI_AVX2_MultiplyAddScalar : NI_AVX2_MultiplyAdd;
    }

    node->ChangeHWIntrinsicId(intrinsic);
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsic: Perform containment analysis for a hardware intrinsic node.
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsic(GenTreeHWIntrinsic* node)
{
    if (node->TypeIs(TYP_SIMD12))
    {
        // GT_HWINTRINSIC node requiring to produce TYP_SIMD12 in fact
        // produces a TYP_SIMD16 result
        node->gtType = TYP_SIMD16;
    }

    NamedIntrinsic intrinsicId = node->GetHWIntrinsicId();

    if (node->OperIsEmbRoundingEnabled())
    {
        size_t   numArgs = node->GetOperandCount();
        GenTree* lastOp  = node->Op(numArgs);
        uint8_t  mode    = 0xFF;

        if (lastOp->IsCnsIntOrI())
        {
            // Mark the constant as contained since it's specially encoded
            MakeSrcContained(node, lastOp);

            mode = static_cast<uint8_t>(lastOp->AsIntCon()->IconValue());
        }

        if ((mode & 0x03) != 0x00)
        {
            // Embedded rounding only works for register-to-register operations, so skip containment
            return node->gtNext;
        }
    }

    bool       isScalar = false;
    genTreeOps oper     = node->GetOperForHWIntrinsicId(&isScalar);

    if (GenTreeHWIntrinsic::OperIsBitwiseHWIntrinsic(oper) && !varTypeIsMask(node))
    {
        // These are the control bytes used for TernaryLogic

        const uint8_t A = 0xF0;
        const uint8_t B = 0xCC;
        const uint8_t C = 0xAA;

        var_types simdType     = node->TypeGet();
        var_types simdBaseType = node->GetSimdBaseType();
        unsigned  simdSize     = node->GetSimdSize();

        GenTree* op1 = node->Op(1);
        GenTree* op2 = node->Op(2);
        GenTree* op3 = nullptr;

        // We want to specially recognize this pattern as GT_NOT
        bool isOperNot = (oper == GT_XOR) && op2->IsVectorAllBitsSet();

        LIR::Use use;
        if (BlockRange().TryGetUse(node, &use))
        {
            GenTree* user = use.User();

            if (user->OperIsHWIntrinsic())
            {
                GenTreeHWIntrinsic* userIntrin = user->AsHWIntrinsic();

                bool       userIsScalar = false;
                genTreeOps userOper     = userIntrin->GetOperForHWIntrinsicId(&isScalar);

                // userIntrin may have re-interpreted the base type
                //
                simdBaseType = userIntrin->GetSimdBaseType();

                if (GenTreeHWIntrinsic::OperIsBitwiseHWIntrinsic(userOper))
                {
                    if (isOperNot && (userOper == GT_AND))
                    {
                        // We want to specially handle GT_AND_NOT as its available without EVEX
                        GenTree* nextNode = node->gtNext;

                        BlockRange().Remove(op2);
                        BlockRange().Remove(node);

                        // Note that despite its name, the xarch instruction does ~op1 & op2, so
                        // we need to ensure op1 is the value whose ones complement is computed

                        op2 = userIntrin->Op(2);

                        if (op2 == node)
                        {
                            op2 = userIntrin->Op(1);
                        }

                        NamedIntrinsic intrinsic =
                            GenTreeHWIntrinsic::GetHWIntrinsicIdForBinOp(comp, GT_AND_NOT, op1, op2, simdBaseType,
                                                                         simdSize, false);

                        userIntrin->ResetHWIntrinsicId(intrinsic, comp, op1, op2);

                        return nextNode;
                    }

                    if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX512))
                    {
                        // For everything else we want to lower it to a standard TernaryLogic node
                        GenTree* nextNode = node->gtNext;

                        BlockRange().Remove(node);
                        op3 = userIntrin->Op(2);

                        // Tracks which two operands get used first
                        TernaryLogicUseFlags firstOpUseFlags = TernaryLogicUseFlags::AB;

                        if (op3 == node)
                        {
                            if (userOper == GT_AND_NOT)
                            {
                                op3 = op2;
                                op2 = op1;
                                op1 = userIntrin->Op(1);

                                // AND_NOT isn't commutative so we need to shift parameters down
                                firstOpUseFlags = TernaryLogicUseFlags::BC;
                            }
                            else
                            {
                                op3 = userIntrin->Op(1);
                            }
                        }

                        uint8_t controlByte = 0x00;

                        if ((userOper == GT_XOR) && op3->IsVectorAllBitsSet())
                        {
                            // We have XOR(OP(A, B), AllBitsSet)
                            //   A: op1
                            //   B: op2
                            //   C: op3 (AllBitsSet)
                            //
                            // We want A to be the unused parameter so swap it around
                            //   A: op3 (AllBitsSet)
                            //   B: op1
                            //   C: op2
                            //
                            // This gives us NOT(OP(B, C))

                            assert(firstOpUseFlags == TernaryLogicUseFlags::AB);

                            std::swap(op2, op3);
                            std::swap(op1, op2);

                            if (isOperNot)
                            {
                                // We have NOT(XOR(B, AllBitsSet))
                                //   A: op3 (AllBitsSet)
                                //   B: op1
                                //   C: op2 (AllBitsSet)
                                //
                                // This represents a double not, so so just return op2
                                // which is the only actual value now that the parameters
                                // were shifted around

                                assert(op1->IsVectorAllBitsSet());
                                assert(op3->IsVectorAllBitsSet());

                                LIR::Use superUse;
                                if (BlockRange().TryGetUse(user, &superUse))
                                {
                                    superUse.ReplaceWith(op2);
                                }
                                else
                                {
                                    op2->SetUnusedValue();
                                }

                                // Since we have a double negation, it's possible that gtNext
                                // is op1 or user. If it is op1, then it's also possible the
                                // subsequent gtNext is user. We need to make sure to skip both
                                // in such a scenario since we're removing them.

                                if (nextNode == op1)
                                {
                                    nextNode = nextNode->gtNext;
                                }

                                if (nextNode == user)
                                {
                                    nextNode = nextNode->gtNext;
                                }

                                BlockRange().Remove(op3);
                                BlockRange().Remove(op1);
                                BlockRange().Remove(user);

                                return nextNode;
                            }
                            else
                            {
                                // We're now doing NOT(OP(B, C))
                                assert(op1->IsVectorAllBitsSet());

                                controlByte = TernaryLogicInfo::GetTernaryControlByte(oper, B, C);
                                controlByte = static_cast<uint8_t>(~controlByte);
                            }
                        }
                        else if (isOperNot)
                        {
                            if (firstOpUseFlags == TernaryLogicUseFlags::AB)
                            {
                                // We have OP(XOR(A, AllBitsSet), C)
                                //   A: op1
                                //   B: op2 (AllBitsSet)
                                //   C: op3
                                //
                                // We want A to be the unused parameter so swap it around
                                //   A: op2 (AllBitsSet)
                                //   B: op1
                                //   C: op3
                                //
                                // This gives us OP(NOT(B), C)

                                assert(op2->IsVectorAllBitsSet());
                                std::swap(op1, op2);

                                controlByte = static_cast<uint8_t>(~B);
                                controlByte = TernaryLogicInfo::GetTernaryControlByte(userOper, controlByte, C);
                            }
                            else
                            {
                                // We have OP(A, XOR(B, AllBitsSet))
                                //   A: op1
                                //   B: op2
                                //   C: op3 (AllBitsSet)
                                //
                                // We want A to be the unused parameter so swap it around
                                //   A: op3 (AllBitsSet)
                                //   B: op1
                                //   C: op2
                                //
                                // This gives us OP(B, NOT(C))

                                assert(firstOpUseFlags == TernaryLogicUseFlags::BC);

                                assert(op3->IsVectorAllBitsSet());
                                std::swap(op2, op3);
                                std::swap(op1, op2);

                                controlByte = static_cast<uint8_t>(~C);
                                controlByte = TernaryLogicInfo::GetTernaryControlByte(userOper, B, controlByte);
                            }
                        }
                        else if (firstOpUseFlags == TernaryLogicUseFlags::AB)
                        {
                            // We have OP2(OP1(A, B), C)
                            controlByte = TernaryLogicInfo::GetTernaryControlByte(oper, A, B);
                            controlByte = TernaryLogicInfo::GetTernaryControlByte(userOper, controlByte, C);
                        }
                        else
                        {
                            // We have OP2(A, OP1(B, C))
                            assert(firstOpUseFlags == TernaryLogicUseFlags::BC);

                            controlByte = TernaryLogicInfo::GetTernaryControlByte(oper, B, C);
                            controlByte = TernaryLogicInfo::GetTernaryControlByte(userOper, A, controlByte);
                        }

                        NamedIntrinsic ternaryLogicId = NI_AVX512_TernaryLogic;

                        GenTree* op4 = comp->gtNewIconNode(controlByte);
                        BlockRange().InsertBefore(userIntrin, op4);

                        userIntrin->ResetHWIntrinsicId(ternaryLogicId, comp, op1, op2, op3, op4);
                        return nextNode;
                    }
                }
            }
        }

        if (isOperNot && comp->compOpportunisticallyDependsOn(InstructionSet_AVX512))
        {
            // Lowering this to TernaryLogic(zero, zero, op1, ~C) is smaller
            // and faster than emitting the pcmpeqd; pxor sequence.

            BlockRange().Remove(op2);

            if (op1->OperIsHWIntrinsic())
            {
                GenTreeHWIntrinsic* opIntrin = op1->AsHWIntrinsic();

                if (opIntrin->GetHWIntrinsicId() == NI_AVX512_TernaryLogic)
                {
                    GenTree* opControl = opIntrin->Op(4);

                    if (opControl->IsCnsIntOrI())
                    {
                        // When the input is already a ternary logic node, we want to invert it rather
                        // than introduce a new ternary logic node.

                        GenTree* nextNode = node->gtNext;

                        GenTreeIntConCommon* opControlCns = opControl->AsIntConCommon();
                        opControlCns->SetIconValue(static_cast<uint8_t>(~opControlCns->IconValue()));

                        if (BlockRange().TryGetUse(node, &use))
                        {
                            use.ReplaceWith(op1);
                        }
                        else
                        {
                            op1->SetUnusedValue();
                        }

                        BlockRange().Remove(node);
                        return nextNode;
                    }
                }
            }

            NamedIntrinsic ternaryLogicId = NI_AVX512_TernaryLogic;

            op3 = op1;

            op2 = comp->gtNewZeroConNode(simdType);
            BlockRange().InsertBefore(node, op2);

            op1 = comp->gtNewZeroConNode(simdType);
            BlockRange().InsertBefore(node, op1);

            GenTree* control = comp->gtNewIconNode(static_cast<uint8_t>(~C));
            BlockRange().InsertBefore(node, control);

            node->ResetHWIntrinsicId(ternaryLogicId, comp, op1, op2, op3, control);
            return LowerNode(node);
        }
    }

    switch (intrinsicId)
    {
        case NI_Vector128_ConditionalSelect:
        case NI_Vector256_ConditionalSelect:
        case NI_Vector512_ConditionalSelect:
        {
            return LowerHWIntrinsicCndSel(node);
        }

        case NI_Vector128_Create:
        case NI_Vector256_Create:
        case NI_Vector512_Create:
        case NI_Vector128_CreateScalar:
        case NI_Vector256_CreateScalar:
        case NI_Vector512_CreateScalar:
        {
            // We don't directly support the Vector128.Create or Vector256.Create methods in codegen
            // and instead lower them to other intrinsic nodes in LowerHWIntrinsicCreate so we expect
            // that the node is modified to either not be a HWIntrinsic node or that it is no longer
            // the same intrinsic as when it came in. In the case of Vector256.Create, we may lower
            // it into 2x Vector128.Create intrinsics which themselves are also lowered into other
            // intrinsics that are not Vector*.Create

            return LowerHWIntrinsicCreate(node);
        }

        case NI_Vector128_Dot:
        case NI_Vector256_Dot:
        {
            return LowerHWIntrinsicDot(node);
        }

        case NI_Vector128_GetElement:
        case NI_Vector256_GetElement:
        case NI_Vector512_GetElement:
        {
            return LowerHWIntrinsicGetElement(node);
        }

        case NI_Vector256_GetUpper:
        {
            assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));
            var_types simdBaseType = node->GetSimdBaseType();

            if (varTypeIsFloating(simdBaseType) || !comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
            {
                intrinsicId = NI_AVX_ExtractVector128;
            }
            else
            {
                intrinsicId = NI_AVX2_ExtractVector128;
            }

            GenTree* op1 = node->Op(1);

            GenTree* op2 = comp->gtNewIconNode(1);
            BlockRange().InsertBefore(node, op2);
            LowerNode(op2);

            node->ResetHWIntrinsicId(intrinsicId, comp, op1, op2);
            break;
        }

        case NI_Vector512_GetUpper:
        {
            assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX512));
            var_types simdBaseType = node->GetSimdBaseType();

            intrinsicId = NI_AVX512_ExtractVector256;

            GenTree* op1 = node->Op(1);

            GenTree* op2 = comp->gtNewIconNode(1);
            BlockRange().InsertBefore(node, op2);
            LowerNode(op2);

            node->ResetHWIntrinsicId(intrinsicId, comp, op1, op2);
            break;
        }

        case NI_Vector128_WithElement:
        case NI_Vector256_WithElement:
        case NI_Vector512_WithElement:
        {
            return LowerHWIntrinsicWithElement(node);
        }

        case NI_Vector256_WithLower:
        case NI_Vector256_WithUpper:
        {
            assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));
            var_types simdBaseType = node->GetSimdBaseType();
            int       index        = (intrinsicId == NI_Vector256_WithUpper) ? 1 : 0;

            if (varTypeIsFloating(simdBaseType) || !comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
            {
                intrinsicId = NI_AVX_InsertVector128;
            }
            else
            {
                intrinsicId = NI_AVX2_InsertVector128;
            }

            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            GenTree* op3 = comp->gtNewIconNode(index);
            BlockRange().InsertBefore(node, op3);
            LowerNode(op3);

            node->ResetHWIntrinsicId(intrinsicId, comp, op1, op2, op3);
            break;
        }

        case NI_Vector512_WithLower:
        case NI_Vector512_WithUpper:
        {
            assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX512));
            var_types simdBaseType = node->GetSimdBaseType();
            int       index        = (intrinsicId == NI_Vector512_WithUpper) ? 1 : 0;

            intrinsicId = NI_AVX512_InsertVector256;

            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            GenTree* op3 = comp->gtNewIconNode(index);
            BlockRange().InsertBefore(node, op3);
            LowerNode(op3);

            node->ResetHWIntrinsicId(intrinsicId, comp, op1, op2, op3);
            break;
        }

        case NI_Vector128_op_Equality:
        case NI_Vector256_op_Equality:
        case NI_Vector512_op_Equality:
        {
            return LowerHWIntrinsicCmpOp(node, GT_EQ);
        }

        case NI_Vector128_op_Inequality:
        case NI_Vector256_op_Inequality:
        case NI_Vector512_op_Inequality:
        {
            return LowerHWIntrinsicCmpOp(node, GT_NE);
        }

        case NI_AVX512_Fixup:
        case NI_AVX512_FixupScalar:
        {
            if (!node->isRMWHWIntrinsic(comp))
            {
                GenTree* op1 = node->Op(1);

                if (!op1->IsCnsVec())
                {
                    // op1 is never selected by the table so
                    // we replaced it with a containable constant

                    var_types simdType = node->TypeGet();

                    op1->SetUnusedValue();
                    op1 = comp->gtNewZeroConNode(simdType);

                    BlockRange().InsertBefore(node, op1);
                    node->Op(1) = op1;
                }
            }
            break;
        }

        case NI_AVX512_CompareEqualMask:
        case NI_AVX512_CompareNotEqualMask:
        {
            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            if (!varTypeIsFloating(node->GetSimdBaseType()) && op2->IsVectorZero())
            {
                NamedIntrinsic testIntrinsicId;

                if (intrinsicId == NI_AVX512_CompareEqualMask)
                {
                    // We have `CompareEqual(x, Zero)` where a given element
                    // equaling zero returns 1. We can therefore use `vptestnm(x, x)`
                    // since it does `(x & x) == 0`, thus giving us `1` if zero and `0`
                    // if non-zero

                    testIntrinsicId = NI_AVX512_PTESTNM;
                }
                else
                {
                    // We have `CompareNotEqual(x, Zero)` where a given element
                    // equaling zero returns 0. We can therefore use `vptestm(x, x)`
                    // since it does `(x & x) != 0`, thus giving us `1` if non-zero and `0`
                    // if zero

                    assert(intrinsicId == NI_AVX512_CompareNotEqualMask);
                    testIntrinsicId = NI_AVX512_PTESTM;
                }

                node->Op(1) = op1;
                BlockRange().Remove(op2);

                LIR::Use op1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(op1Use);
                op1 = node->Op(1);

                op2 = comp->gtClone(op1);
                BlockRange().InsertAfter(op1, op2);
                node->Op(2) = op2;

                node->ChangeHWIntrinsicId(testIntrinsicId);
                return LowerNode(node);
            }
            break;
        }

        case NI_AVX512_AndMask:
        {
            // We want to recognize (~op1 & op2) and transform it
            // into Evex.AndNotMask(op1, op2) as well as (op1 & ~op2)
            // transforming it into Evex.AndNotMask(op2, op1), which
            // takes into account that the XARCH APIs operate more like
            // NotAnd

            bool transform = false;

            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);

            if (op1->OperIsHWIntrinsic(NI_AVX512_NotMask))
            {
                GenTreeHWIntrinsic* opIntrin         = op1->AsHWIntrinsic();
                unsigned            simdBaseTypeSize = genTypeSize(node->GetSimdBaseType());

                if (genTypeSize(opIntrin->GetSimdBaseType()) == simdBaseTypeSize)
                {
                    transform = true;

                    op1 = opIntrin->Op(1);
                    BlockRange().Remove(opIntrin);
                }
            }

            if (!transform && op2->OperIsHWIntrinsic(NI_AVX512_NotMask))
            {
                GenTreeHWIntrinsic* opIntrin         = op2->AsHWIntrinsic();
                unsigned            simdBaseTypeSize = genTypeSize(node->GetSimdBaseType());

                if (genTypeSize(opIntrin->GetSimdBaseType()) == simdBaseTypeSize)
                {
                    transform = true;

                    op2 = opIntrin->Op(1);
                    BlockRange().Remove(opIntrin);

                    std::swap(op1, op2);
                }
            }

            if (transform)
            {
                intrinsicId = NI_AVX512_AndNotMask;
                node->ChangeHWIntrinsicId(intrinsicId, op1, op2);
            }
            break;
        }

        case NI_AVX512_NotMask:
        {
            // We want to recognize ~(op1 ^ op2) and transform it
            // into Evex.XnorMask(op1, op2)

            GenTree* op1 = node->Op(1);

            if (op1->OperIsHWIntrinsic(NI_AVX512_XorMask))
            {
                GenTreeHWIntrinsic* opIntrin         = op1->AsHWIntrinsic();
                unsigned            simdBaseTypeSize = genTypeSize(node->GetSimdBaseType());

                if (genTypeSize(opIntrin->GetSimdBaseType()) == simdBaseTypeSize)
                {
                    intrinsicId = NI_AVX512_XnorMask;
                    node->ResetHWIntrinsicId(intrinsicId, comp, opIntrin->Op(1), opIntrin->Op(2));
                    BlockRange().Remove(opIntrin);
                }
            }
            break;
        }

        case NI_Vector128_ToScalar:
        case NI_Vector256_ToScalar:
        case NI_Vector512_ToScalar:
        {
            return LowerHWIntrinsicToScalar(node);
        }

        case NI_SSE42_Extract:
        {
            if (varTypeIsFloating(node->GetSimdBaseType()))
            {
                assert(node->GetSimdBaseType() == TYP_FLOAT);
                assert(node->GetSimdSize() == 16);

                GenTree* op2 = node->Op(2);

                if (!op2->OperIsConst())
                {
                    // Extract allows the full range while GetElement only allows
                    // 0-3, so we need to mask the index here so codegen works.

                    GenTree* msk = comp->gtNewIconNode(3, TYP_INT);
                    BlockRange().InsertAfter(op2, msk);

                    GenTree* tmp = comp->gtNewOperNode(GT_AND, TYP_INT, op2, msk);
                    BlockRange().InsertAfter(msk, tmp);
                    LowerNode(tmp);

                    node->Op(2) = tmp;
                }

                node->ChangeHWIntrinsicId(NI_Vector128_GetElement);
                return LowerNode(node);
            }
            break;
        }

        case NI_SSE42_Insert:
        {
            assert(node->GetOperandCount() == 3);

            if (node->GetSimdBaseType() != TYP_FLOAT)
            {
                break;
            }

            // We have Sse41.Insert in which case we can specially handle
            // a couple of interesting scenarios involving chains of Inserts
            // where one of them involves inserting zero
            //
            // Given Sse41.Insert has an index:
            //  * Bits 0-3: zmask
            //  * Bits 4-5: count_d
            //  * Bits 6-7: count_s (register form only)
            //
            // Where zmask specifies which elements to zero
            // Where count_d specifies the destination index the value is being inserted to
            // Where count_s specifies the source index of the value being inserted
            //
            // We can recognize  `Insert(Insert(vector, zero, index1), value, index2)` and
            // transform it into just `Insert(vector, value, index)`. This is because we
            // can remove the inner insert and update the relevant index fields.
            //
            // We can likewise recognize `Insert(Insert(vector, value, index1), zero, index2)`
            // and do a similar transformation.

            GenTree* op1 = node->Op(1);
            GenTree* op2 = node->Op(2);
            GenTree* op3 = node->Op(3);

            bool op1IsVectorZero = op1->IsVectorZero();
            bool op2IsVectorZero = op2->IsVectorZero();

            if (op1IsVectorZero && op2IsVectorZero)
            {
                // We need to change op1's type to the return type of the node in this case.
                // We have to do this because we are going to propagate the constant up.
                op1->ChangeType(node->TypeGet());

                // Ensure the upper values are zero by zero-initialization.
                op1->AsVecCon()->gtSimdVal = {};

                // While this case is unlikely, we'll handle it here to simplify some
                // of the logic that exists below. Effectively `Insert(zero, zero, idx)`
                // is always going to produce zero, so we'll just replace ourselves with
                // zero. This ensures we don't need to handle a case where op2 is zero
                // but not contained.

                GenTree* nextNode = node->gtNext;

                LIR::Use use;

                if (BlockRange().TryGetUse(node, &use))
                {
                    use.ReplaceWith(op1);
                }
                else
                {
                    op1->SetUnusedValue();
                }

                BlockRange().Remove(op2);
                op3->SetUnusedValue();
                BlockRange().Remove(node);

                return nextNode;
            }

            if (!op3->IsCnsIntOrI())
            {
                // Nothing to do if op3 isn't a constant
                break;
            }

            ssize_t ival = op3->AsIntConCommon()->IconValue();

            ssize_t zmask   = (ival & 0x0F);
            ssize_t count_d = (ival & 0x30) >> 4;
            ssize_t count_s = (ival & 0xC0) >> 6;

            if (op1IsVectorZero)
            {
                // When op1 is zero, we can modify the mask to zero
                // everything except for the element we're inserting

                zmask |= ~(ssize_t(1) << count_d);
                zmask &= 0x0F;

                ival = (count_s << 6) | (count_d << 4) | (zmask);
                op3->AsIntConCommon()->SetIconValue(ival);
            }
            else if (op2IsVectorZero)
            {
                // When op2 is zero, we can modify the mask to
                // directly zero the element we're inserting

                zmask |= (ssize_t(1) << count_d);
                zmask &= 0x0F;

                ival = (count_s << 6) | (count_d << 4) | (zmask);
                op3->AsIntConCommon()->SetIconValue(ival);
            }

            if (zmask == 0x0F)
            {
                // This is another unlikely case, we'll handle it here to simplify some
                // of the logic that exists below. In this case, the zmask says all entries
                // should be zeroed out, so we'll just replace ourselves with zero.

                GenTree* nextNode = node->gtNext;

                LIR::Use use;

                if (BlockRange().TryGetUse(node, &use))
                {
                    GenTree* zeroNode = comp->gtNewZeroConNode(TYP_SIMD16);
                    BlockRange().InsertBefore(node, zeroNode);
                    use.ReplaceWith(zeroNode);
                }
                else
                {
                    // We're an unused zero constant node, so don't both creating
                    // a new node for something that will never be consumed
                }

                op1->SetUnusedValue();
                op2->SetUnusedValue();
                op3->SetUnusedValue();
                BlockRange().Remove(node);

                return nextNode;
            }

            if (!op1->OperIsHWIntrinsic())
            {
                // Nothing to do if op1 isn't an intrinsic
                break;
            }

            GenTreeHWIntrinsic* op1Intrinsic = op1->AsHWIntrinsic();

            if ((op1Intrinsic->GetHWIntrinsicId() != NI_SSE42_Insert) || (op1Intrinsic->GetSimdBaseType() != TYP_FLOAT))
            {
                // Nothing to do if op1 isn't a float32 Sse41.Insert
                break;
            }

            GenTree* op1Idx = op1Intrinsic->Op(3);

            if (!op1Idx->IsCnsIntOrI())
            {
                // Nothing to do if op1's index isn't a constant
                break;
            }

            if (!IsInvariantInRange(op1, node))
            {
                // What we're doing here is effectively similar to containment,
                // except for we're deleting the node entirely, so don't we have
                // nothing to do if there are side effects between node and op1
                break;
            }

            if (op1Intrinsic->Op(2)->IsVectorZero())
            {
                // First build up the new index by updating zmask to include
                // the zmask from op1. We expect that op2 has already been
                // lowered and therefore the containment checks have happened

                // Since this is a newer operation, we need to account for
                // the possibility of `op1Intrinsic` zeroing the same element
                // we're setting here.

                assert(op1Intrinsic->Op(2)->isContained());

                ssize_t op1Ival = op1Idx->AsIntConCommon()->IconValue();
                ival |= ((op1Ival & 0x0F) & ~(1 << count_d));
                op3->AsIntConCommon()->SetIconValue(ival);

                // Then we'll just carry the original non-zero input and
                // remove the now unused constant nodes

                node->Op(1) = op1Intrinsic->Op(1);

                BlockRange().Remove(op1Intrinsic->Op(2));
                BlockRange().Remove(op1Intrinsic->Op(3));
                BlockRange().Remove(op1Intrinsic);
            }
            else if (op2IsVectorZero)
            {
                // Since we've already updated zmask to take op2 being zero into
                // account, we can basically do the same thing here by merging this
                // zmask into the ival from op1.

                // Since this is a later op, direct merging is safe

                ssize_t op1Ival = op1Idx->AsIntConCommon()->IconValue();
                ival            = op1Ival | zmask;
                op3->AsIntConCommon()->SetIconValue(ival);

                // Then we'll just carry the inputs from op1 and remove the now
                // unused constant nodes

                node->Op(1) = op1Intrinsic->Op(1);
                node->Op(2) = op1Intrinsic->Op(2);

                BlockRange().Remove(op2);
                BlockRange().Remove(op1Intrinsic->Op(3));
                BlockRange().Remove(op1Intrinsic);
            }
            break;
        }

        case NI_X86Base_CompareGreaterThan:
        case NI_X86Base_CompareGreaterThanOrEqual:
        case NI_X86Base_CompareNotGreaterThan:
        case NI_X86Base_CompareNotGreaterThanOrEqual:
        {
            if (!varTypeIsFloating(node->GetSimdBaseType()))
            {
                assert(varTypeIsIntegral(node->GetSimdBaseType()));
                break;
            }

            if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX))
            {
                break;
            }

            // pre-AVX doesn't actually support these intrinsics in hardware so we need to swap the operands around
            NamedIntrinsic newIntrinsicId = NI_Illegal;

            switch (intrinsicId)
            {
                case NI_X86Base_CompareGreaterThan:
                {
                    newIntrinsicId = NI_X86Base_CompareLessThan;
                    break;
                }

                case NI_X86Base_CompareGreaterThanOrEqual:
                {
                    newIntrinsicId = NI_X86Base_CompareLessThanOrEqual;
                    break;
                }

                case NI_X86Base_CompareNotGreaterThan:
                {
                    newIntrinsicId = NI_X86Base_CompareNotLessThan;
                    break;
                }

                case NI_X86Base_CompareNotGreaterThanOrEqual:
                {
                    newIntrinsicId = NI_X86Base_CompareNotLessThanOrEqual;
                    break;
                }

                default:
                {
                    unreached();
                }
            }

            assert(newIntrinsicId != NI_Illegal);
            assert(intrinsicId != newIntrinsicId);

            node->ChangeHWIntrinsicId(newIntrinsicId);
            std::swap(node->Op(1), node->Op(2));
            break;
        }

        case NI_X86Base_CompareLessThan:
        case NI_SSE42_CompareLessThan:
        case NI_AVX2_CompareLessThan:
        {
            if (varTypeIsFloating(node->GetSimdBaseType()))
            {
                break;
            }
            assert(varTypeIsIntegral(node->GetSimdBaseType()));

            // pre-AVX512 doesn't actually support these intrinsics in hardware so we need to swap the operands around
            NamedIntrinsic newIntrinsicId = NI_Illegal;

            switch (intrinsicId)
            {
                case NI_X86Base_CompareLessThan:
                {
                    newIntrinsicId = NI_X86Base_CompareGreaterThan;
                    break;
                }

                case NI_SSE42_CompareLessThan:
                {
                    newIntrinsicId = NI_SSE42_CompareGreaterThan;
                    break;
                }

                case NI_AVX2_CompareLessThan:
                {
                    newIntrinsicId = NI_AVX2_CompareGreaterThan;
                    break;
                }

                default:
                {
                    unreached();
                }
            }

            assert(newIntrinsicId != NI_Illegal);
            assert(intrinsicId != newIntrinsicId);

            node->ChangeHWIntrinsicId(newIntrinsicId);
            std::swap(node->Op(1), node->Op(2));
            break;
        }

        case NI_X86Base_CompareScalarOrderedEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_COMIS, GenCondition::FEQ);
            break;
        case NI_X86Base_CompareScalarOrderedNotEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_COMIS, GenCondition::FNEU);
            break;
        case NI_X86Base_CompareScalarOrderedLessThan:
            LowerHWIntrinsicCC(node, NI_X86Base_COMIS, GenCondition::FLT);
            break;
        case NI_X86Base_CompareScalarOrderedLessThanOrEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_COMIS, GenCondition::FLE);
            break;
        case NI_X86Base_CompareScalarOrderedGreaterThan:
            LowerHWIntrinsicCC(node, NI_X86Base_COMIS, GenCondition::FGT);
            break;
        case NI_X86Base_CompareScalarOrderedGreaterThanOrEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_COMIS, GenCondition::FGE);
            break;

        case NI_X86Base_CompareScalarUnorderedEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_UCOMIS, GenCondition::FEQ);
            break;
        case NI_X86Base_CompareScalarUnorderedNotEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_UCOMIS, GenCondition::FNEU);
            break;
        case NI_X86Base_CompareScalarUnorderedLessThanOrEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_UCOMIS, GenCondition::FLE);
            break;
        case NI_X86Base_CompareScalarUnorderedLessThan:
            LowerHWIntrinsicCC(node, NI_X86Base_UCOMIS, GenCondition::FLT);
            break;
        case NI_X86Base_CompareScalarUnorderedGreaterThanOrEqual:
            LowerHWIntrinsicCC(node, NI_X86Base_UCOMIS, GenCondition::FGE);
            break;
        case NI_X86Base_CompareScalarUnorderedGreaterThan:
            LowerHWIntrinsicCC(node, NI_X86Base_UCOMIS, GenCondition::FGT);
            break;

        case NI_SSE42_TestC:
            LowerHWIntrinsicCC(node, NI_SSE42_PTEST, GenCondition::C);
            break;
        case NI_SSE42_TestZ:
            LowerHWIntrinsicCC(node, NI_SSE42_PTEST, GenCondition::EQ);
            break;
        case NI_SSE42_TestNotZAndNotC:
            LowerHWIntrinsicCC(node, NI_SSE42_PTEST, GenCondition::UGT);
            break;

        case NI_AVX_TestC:
            LowerHWIntrinsicCC(node, NI_AVX_PTEST, GenCondition::C);
            break;
        case NI_AVX_TestZ:
            LowerHWIntrinsicCC(node, NI_AVX_PTEST, GenCondition::EQ);
            break;
        case NI_AVX_TestNotZAndNotC:
            LowerHWIntrinsicCC(node, NI_AVX_PTEST, GenCondition::UGT);
            break;

        case NI_AVX2_MultiplyAdd:
        case NI_AVX2_MultiplyAddNegated:
        case NI_AVX2_MultiplyAddNegatedScalar:
        case NI_AVX2_MultiplyAddScalar:
        case NI_AVX2_MultiplySubtract:
        case NI_AVX2_MultiplySubtractNegated:
        case NI_AVX2_MultiplySubtractNegatedScalar:
        case NI_AVX2_MultiplySubtractScalar:
        case NI_AVX512_FusedMultiplyAdd:
        case NI_AVX512_FusedMultiplyAddNegated:
        case NI_AVX512_FusedMultiplyAddNegatedScalar:
        case NI_AVX512_FusedMultiplyAddScalar:
        case NI_AVX512_FusedMultiplySubtract:
        case NI_AVX512_FusedMultiplySubtractNegated:
        case NI_AVX512_FusedMultiplySubtractNegatedScalar:
        case NI_AVX512_FusedMultiplySubtractScalar:
        {
            LowerFusedMultiplyOp(node);
            break;
        }

        case NI_AVX512_TernaryLogic:
        {
            return LowerHWIntrinsicTernaryLogic(node);
        }

        case NI_GFNI_GaloisFieldAffineTransform:
        case NI_GFNI_GaloisFieldAffineTransformInverse:
        case NI_GFNI_V256_GaloisFieldAffineTransform:
        case NI_GFNI_V256_GaloisFieldAffineTransformInverse:
        case NI_GFNI_V512_GaloisFieldAffineTransform:
        case NI_GFNI_V512_GaloisFieldAffineTransformInverse:
        {
            // Managed API surfaces these with only UBYTE operands.
            // We retype in order to support EVEX embedded broadcast of op2
            node->SetSimdBaseJitType(CORINFO_TYPE_ULONG);
            break;
        }

        default:
            break;
    }

    ContainCheckHWIntrinsic(node);
    return node->gtNext;
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicCmpOp: Lowers a Vector128 or Vector256 comparison intrinsic
//
//  Arguments:
//     node  - The hardware intrinsic node.
//     cmpOp - The comparison operation, currently must be GT_EQ or GT_NE
//
GenTree* Lowering::LowerHWIntrinsicCmpOp(GenTreeHWIntrinsic* node, genTreeOps cmpOp)
{
    NamedIntrinsic intrinsicId     = node->GetHWIntrinsicId();
    CorInfoType    simdBaseJitType = node->GetSimdBaseJitType();
    var_types      simdBaseType    = node->GetSimdBaseType();
    unsigned       simdSize        = node->GetSimdSize();
    var_types      simdType        = Compiler::getSIMDTypeForSize(simdSize);

    assert((intrinsicId == NI_Vector128_op_Equality) || (intrinsicId == NI_Vector128_op_Inequality) ||
           (intrinsicId == NI_Vector256_op_Equality) || (intrinsicId == NI_Vector256_op_Inequality) ||
           (intrinsicId == NI_Vector512_op_Equality) || (intrinsicId == NI_Vector512_op_Inequality));

    assert(varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);
    assert(node->TypeIs(TYP_INT));
    assert((cmpOp == GT_EQ) || (cmpOp == GT_NE));

    // We have the following (with the appropriate simd size and where the intrinsic could be op_Inequality):
    //          /--*  op2  simd
    //          /--*  op1  simd
    //   node = *  HWINTRINSIC   simd   T op_Equality

    GenTree*     op1    = node->Op(1);
    GenTree*     op1Msk = op1;
    GenTree*     op2    = node->Op(2);
    GenCondition cmpCnd = (cmpOp == GT_EQ) ? GenCondition::EQ : GenCondition::NE;

    // We may need to change the base type to match the underlying mask size to ensure
    // the right instruction variant is picked. If the op_Equality was for TYP_INT but
    // the mask was for TYP_DOUBLE then we'd pick kortestw when we really want kortestb.
    // Changing the size is fine in some scenarios, such as comparison against Zero or
    // AllBitsSet, but not in other scenarios such as against an arbitrary mask.

    CorInfoType maskBaseJitType = simdBaseJitType;
    var_types   maskBaseType    = simdBaseType;

    if (op1Msk->OperIsConvertMaskToVector())
    {
        GenTreeHWIntrinsic* cvtMaskToVector = op1Msk->AsHWIntrinsic();

        op1Msk = cvtMaskToVector->Op(1);
        assert(varTypeIsMask(op1Msk));

        maskBaseJitType = cvtMaskToVector->GetSimdBaseJitType();
        maskBaseType    = cvtMaskToVector->GetSimdBaseType();
    }

    if (!varTypeIsFloating(simdBaseType) && (simdSize != 64) && !varTypeIsMask(op1Msk))
    {
        bool isOp2VectorZero = op2->IsVectorZero();

        if ((isOp2VectorZero || op2->IsVectorAllBitsSet()) &&
            comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
        {
            // On SSE4.2 or higher we can optimize comparisons against Zero or AllBitsSet to
            // just use PTEST. We can't support it for floating-point, however, as it has
            // both +0.0 and -0.0 where +0.0 == -0.0

            bool skipReplaceOperands = false;

            if (!isOp2VectorZero)
            {
                // We can optimize to TestC(op1, allbitsset)
                //
                // This works out because TestC sets CF if (~x & y) == 0, so:
                //   ~00 & 11 = 11;  11 & 11 = 11;  NC
                //   ~01 & 11 = 01;  10 & 11 = 10;  NC
                //   ~10 & 11 = 10;  01 & 11 = 01;  NC
                //   ~11 & 11 = 11;  00 & 11 = 00;  C

                assert(op2->IsVectorAllBitsSet());
                cmpCnd = (cmpOp == GT_EQ) ? GenCondition::C : GenCondition::NC;

                skipReplaceOperands = true;
            }
            else if (op1->OperIsHWIntrinsic())
            {
                assert(op2->IsVectorZero());

                GenTreeHWIntrinsic* op1Intrinsic = op1->AsHWIntrinsic();

                if (op1Intrinsic->GetOperandCount() == 2)
                {
                    GenTree* nestedOp1 = op1Intrinsic->Op(1);
                    GenTree* nestedOp2 = op1Intrinsic->Op(2);

                    assert(!nestedOp1->isContained());
                    bool isEmbeddedBroadcast = nestedOp2->isContained() && nestedOp2->OperIsHWIntrinsic();

                    bool       isScalar = false;
                    genTreeOps oper     = op1Intrinsic->GetOperForHWIntrinsicId(&isScalar);

                    switch (oper)
                    {
                        case GT_AND:
                        {
                            // We can optimize to TestZ(op1.op1, op1.op2)

                            if (isEmbeddedBroadcast)
                            {
                                // PTEST doesn't support embedded broadcast
                                break;
                            }

                            node->Op(1) = nestedOp1;
                            node->Op(2) = nestedOp2;

                            BlockRange().Remove(op1);
                            BlockRange().Remove(op2);

                            skipReplaceOperands = true;
                            break;
                        }

                        case GT_AND_NOT:
                        {
                            // We can optimize to TestC(op1.op1, op1.op2)

                            if (isEmbeddedBroadcast)
                            {
                                // PTEST doesn't support embedded broadcast
                                break;
                            }

                            cmpCnd = (cmpOp == GT_EQ) ? GenCondition::C : GenCondition::NC;

                            node->Op(1) = nestedOp1;
                            node->Op(2) = nestedOp2;

                            BlockRange().Remove(op1);
                            BlockRange().Remove(op2);

                            skipReplaceOperands = true;
                            break;
                        }

                        default:
                        {
                            break;
                        }
                    }
                }
            }

            if (!skipReplaceOperands)
            {
                // Default handler, emit a TestZ(op1, op1)
                assert(op2->IsVectorZero());

                node->Op(1) = op1;
                BlockRange().Remove(op2);

                LIR::Use op1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(op1Use);
                op1 = node->Op(1);

                op2 = comp->gtClone(op1);
                BlockRange().InsertAfter(op1, op2);
                node->Op(2) = op2;
            }

            if (simdSize == 32)
            {
                LowerHWIntrinsicCC(node, NI_AVX_PTEST, cmpCnd);
            }
            else
            {
                assert(simdSize == 16);
                LowerHWIntrinsicCC(node, NI_SSE42_PTEST, cmpCnd);
            }
            return LowerNode(node);
        }
    }

    // TODO-XARCH-AVX512: We should handle TYP_SIMD12 here under the EVEX path, but doing
    // so will require us to account for the unused 4th element.

    if ((simdType != TYP_SIMD12) && comp->canUseEvexEncoding())
    {
        // The EVEX encoded versions of the comparison instructions all return a kmask
        //
        // For the comparisons against zero that we normally optimize to use `PTEST` we
        // have to make a decision to use EVEX and emit 2 instructions (vpcmp + kortest)
        // or to continue emitting PTEST and hope that the register allocator isn't limited
        // by it not supporting the extended register set.
        //
        // Ideally we'd opt to not use PTEST when EVEX is available, This would be done so we can
        // best take advantage of EVEX exclusive features such as embedded broadcast and the
        // 16 additional registers. In many cases this allows for overall denser codegen where
        // we are doing more in the same number of bytes, even though the individual instruction
        // is 1-2 bytes larger. Even though there may be cases where continuing to use PTEST for select-
        // 128/256-bit code paths would still be beneficial, the additional complexity required
        // to detect and account for those differences is not likely to be worth the tradeoff.
        //
        // TODO-XARCH-AVX512: Given the above don't emit the PTEST path above when AVX-512 is available
        // This will require exposing `NI_AVX512_TestZ` so that we can keep codegen optimized to just
        // `vptestm` followed by `kortest`. This will be one instruction more than just `vptest` but
        // it has the advantages detailed above.
        //
        // For other comparisons, using EVEX allows us to avoid leaving the SIMD domain, avoids
        // needing to use a general-purpose register, and allows us to generate less instructions.

        GenTree* maskNode = node;
        GenTree* nextNode = node->gtNext;

        NamedIntrinsic maskIntrinsicId = NI_AVX512_CompareEqualMask;
        uint32_t       count           = simdSize / genTypeSize(maskBaseType);

        // KORTEST does a bitwise or on the result and sets ZF if it is zero and CF if it is all
        // bits set. Because of this, when we have at least 8 elements to compare we can use a
        // normal comparison alongside CF.
        //
        // That is, if the user wants `x == y`, we can keep it as `mask = (x == y)` and then emit
        // `kortest mask, mask` and check `CF == 1`. This will be true if all elements matched and
        // false otherwise. Things work out nicely and we keep readable disasm.
        //
        // Likewise, if the user wants `x != y`, we can keep it as `mask = (x != y)` and then emit
        // `kortest mask, mask` and check `ZF != 0`. This will be true if any elements mismatched.
        //
        // However, if we have less than 8 elements then we have to change it up since we have less
        // than 8 bits in the output mask and unused bits will be set to 0. This occurs for 32-bit
        // for Vector128 and and 64-bit elements when using either Vector128 or Vector256.
        //
        // To account for this, we will invert the comparison being done. So if the user wants
        // `x == y`, we will instead emit `mask = (x != y)`, we will still emit `kortest mask, mask`,
        // but we will then check for `ZF == 0`. This works since that equates to all elements being equal
        //
        // Likewise for `x != y` we will instead emit `mask = (x == y)`, then `kortest mask, mask`,
        // and will then check for `CF == 0` which equates to one or more elements not being equal

        // The scenarios we have to for a full mask are:
        // * No matches:      0000_0000 - ZF == 1, CF == 0
        // * Partial matches: 0000_1111 - ZF == 0, CF == 0
        // * All matches:     1111_1111 - ZF == 0, CF == 1
        //
        // The scenarios we have to for a partial mask are:
        // * No matches:      0000_0000 - ZF == 1, CF == 0
        // * Partial matches: 0000_0011 - ZF == 0, CF == 0
        // * All matches:     0000_1111 - ZF == 0, CF == 0
        //
        // When we have less than a full mask worth of elements, we need to account for the upper
        // bits being implicitly zero. To do that, we may need to invert the comparison.
        //
        // By inverting the comparison we'll get:
        // * All matches:     0000_0000 - ZF == 1, CF == 0
        // * Partial matches: 0000_0011 - ZF == 0, CF == 0
        // * No matches:      0000_1111 - ZF == 0, CF == 0
        //
        // This works since the upper bits are implicitly zero and so by inverting matches also become
        // zero, which in turn means that `AllBitsSet` will become `Zero` and other cases become non-zero

        if (varTypeIsMask(op1Msk) && op2->IsCnsVec())
        {
            // We want to specially handle the common cases of `mask op Zero` and `mask op AllBitsSet`
            //
            // These get created for the various `gtNewSimdCmpOpAnyNode` and `gtNewSimdCmpOpAllNode`
            // scenarios and we want to ensure they still get "optimal" codegen. To handle that, we
            // simply consume the mask directly and preserve the intended comparison by tweaking the
            // compare condition passed down into `KORTEST`

            maskNode = op1Msk;
            assert(maskNode->TypeIs(TYP_MASK));

            bool           isHandled = false;
            GenTreeVecCon* vecCon    = op2->AsVecCon();

            if (vecCon->IsZero())
            {
                // We have `mask == Zero` which is the same as checking that nothing in the mask
                // is set. This scenario can be handled by `kortest` and then checking that `ZF == 1`
                //
                // -or-
                //
                // We have `mask != Zero` which is the same as checking that something in the mask
                // is set. This scenario can be handled by `kortest` and then checking that `ZF == 0`
                //
                // Since this is the default state for `CompareEqualMask` + `GT_EQ`/`GT_NE`, there is nothing
                // for us to change. This also applies to cases where we have less than a full mask of
                // elements since the upper mask bits are implicitly zero.

                isHandled = true;
            }
            else if (vecCon->IsAllBitsSet())
            {
                // We have `mask == AllBitsSet` which is the same as checking that everything in the mask
                // is set. This scenario can be handled by `kortest` and then checking that `CF == 1` for
                // a full mask and checking `ZF == 1` for a partial mask using an inverted comparison
                //
                // -or-
                //
                // We have `mask != AllBitsSet` which is the same as checking that something in the mask
                // is set. This scenario can be handled by `kortest` and then checking that `CF == 0` for
                // a full mask and checking `ZF != 0` for a partial mask using an inverted comparison

                if (count < 8)
                {
                    assert((count == 1) || (count == 2) || (count == 4));

                    maskIntrinsicId = NI_Illegal;

                    if (maskNode->OperIsHWIntrinsic())
                    {
                        GenTreeHWIntrinsic* mskIntrin = maskNode->AsHWIntrinsic();

                        bool       mskIsScalar = false;
                        genTreeOps mskOper =
                            mskIntrin->GetOperForHWIntrinsicId(&mskIsScalar, /* getEffectiveOp */ true);

                        if (GenTree::OperIsCompare(mskOper))
                        {
                            var_types mskSimdBaseType = mskIntrin->GetSimdBaseType();
                            unsigned  mskSimdSize     = mskIntrin->GetSimdSize();

                            GenTree* cmpOp1 = mskIntrin->Op(1);
                            GenTree* cmpOp2 = mskIntrin->Op(2);

                            maskIntrinsicId =
                                GenTreeHWIntrinsic::GetHWIntrinsicIdForCmpOp(comp, mskOper, TYP_MASK, cmpOp1, cmpOp2,
                                                                             mskSimdBaseType, mskSimdSize, mskIsScalar,
                                                                             /* reverseCond */ true);
                        }
                        else
                        {
                            // We have some other comparison intrinsics that don't map to the simple forms
                            NamedIntrinsic mskIntrinsic = mskIntrin->GetHWIntrinsicId();

                            switch (mskIntrinsic)
                            {
                                case NI_AVX_Compare:
                                case NI_AVX512_Compare:
                                case NI_AVX_CompareScalar:
                                {
                                    assert(mskIntrin->GetOperandCount() == 3);

                                    GenTree* cmpOp3 = mskIntrin->Op(3);

                                    if (!cmpOp3->IsCnsIntOrI())
                                    {
                                        break;
                                    }

                                    FloatComparisonMode mode =
                                        static_cast<FloatComparisonMode>(cmpOp3->AsIntConCommon()->IntegralValue());

                                    FloatComparisonMode newMode = mode;

                                    switch (mode)
                                    {
                                        case FloatComparisonMode::UnorderedEqualNonSignaling:
                                        case FloatComparisonMode::UnorderedEqualSignaling:
                                        {
                                            newMode = FloatComparisonMode::OrderedNotEqualNonSignaling;
                                            break;
                                        }

                                        case FloatComparisonMode::OrderedFalseNonSignaling:
                                        case FloatComparisonMode::OrderedFalseSignaling:
                                        {
                                            newMode = FloatComparisonMode::UnorderedTrueNonSignaling;
                                            break;
                                        }

                                        case FloatComparisonMode::OrderedNotEqualNonSignaling:
                                        case FloatComparisonMode::OrderedNotEqualSignaling:
                                        {
                                            newMode = FloatComparisonMode::UnorderedEqualNonSignaling;
                                            break;
                                        }

                                        case FloatComparisonMode::UnorderedTrueNonSignaling:
                                        case FloatComparisonMode::UnorderedTrueSignaling:
                                        {
                                            newMode = FloatComparisonMode::OrderedFalseNonSignaling;
                                            break;
                                        }

                                        default:
                                        {
                                            // Other modes should either have been normalized or
                                            // will be out of range values and can't be handled
                                            break;
                                        }
                                    }

                                    if (newMode != mode)
                                    {
                                        cmpOp3->AsIntConCommon()->SetIntegralValue(static_cast<uint8_t>(mode));
                                        maskIntrinsicId = mskIntrinsic;
                                    }
                                    break;
                                }

                                default:
                                {
                                    break;
                                }
                            }
                        }
                    }

                    if (maskIntrinsicId == NI_Illegal)
                    {
                        // We don't have a well known intrinsic, so we need to inverse the mask keeping the upper
                        // n-bits clear. If we have 1 element, then the upper 7-bits need to be cleared. If we have
                        // 2, then the upper 6-bits, and if we have 4, then the upper 4-bits.
                        //
                        // There isn't necessarily a trivial way to do this outside not, shift-left by n,
                        // shift-right by n. This preserves count bits, while clearing the upper n-bits

                        GenTree* cnsNode;

                        maskNode = comp->gtNewSimdHWIntrinsicNode(TYP_MASK, maskNode, NI_AVX512_NotMask,
                                                                  maskBaseJitType, simdSize);
                        BlockRange().InsertBefore(node, maskNode);

                        cnsNode = comp->gtNewIconNode(8 - count);
                        BlockRange().InsertAfter(maskNode, cnsNode);

                        maskNode = comp->gtNewSimdHWIntrinsicNode(TYP_MASK, maskNode, cnsNode, NI_AVX512_ShiftLeftMask,
                                                                  maskBaseJitType, simdSize);
                        BlockRange().InsertAfter(cnsNode, maskNode);
                        LowerNode(maskNode);

                        cnsNode = comp->gtNewIconNode(8 - count);
                        BlockRange().InsertAfter(maskNode, cnsNode);

                        maskNode = comp->gtNewSimdHWIntrinsicNode(TYP_MASK, maskNode, cnsNode, NI_AVX512_ShiftRightMask,
                                                                  maskBaseJitType, simdSize);
                        BlockRange().InsertAfter(cnsNode, maskNode);

                        maskIntrinsicId = NI_AVX512_ShiftRightMask;
                    }

                    maskNode->AsHWIntrinsic()->ChangeHWIntrinsicId(maskIntrinsicId);
                    LowerNode(maskNode);
                }
                else if (cmpOp == GT_EQ)
                {
                    cmpCnd = GenCondition::C;
                }
                else
                {
                    cmpCnd = GenCondition::NC;
                }
                isHandled = true;
            }

            if (isHandled)
            {
                LIR::Use use;
                if (BlockRange().TryGetUse(node, &use))
                {
                    use.ReplaceWith(maskNode);
                }
                else
                {
                    maskNode->SetUnusedValue();
                }

                BlockRange().Remove(op2);

                if (op1Msk != op1)
                {
                    BlockRange().Remove(op1);
                }

                BlockRange().Remove(node);

                op1 = nullptr;
                op2 = nullptr;
            }
        }

        if (!varTypeIsFloating(simdBaseType) && (op2 != nullptr) && op2->IsVectorZero())
        {
            NamedIntrinsic testIntrinsicId     = NI_AVX512_PTESTM;
            bool           skipReplaceOperands = false;

            if (op1->OperIsHWIntrinsic())
            {
                GenTreeHWIntrinsic* op1Intrinsic   = op1->AsHWIntrinsic();
                NamedIntrinsic      op1IntrinsicId = op1Intrinsic->GetHWIntrinsicId();

                bool       isScalar = false;
                genTreeOps oper     = op1Intrinsic->GetOperForHWIntrinsicId(&isScalar);

                switch (oper)
                {
                    case GT_AND:
                    {
                        // We have `(x & y) == 0` with GenCondition::EQ (jz, setz, cmovz)
                        // or `(x & y) != 0`with GenCondition::NE (jnz, setnz, cmovnz)
                        //
                        // `vptestnm(x, y)` does the equivalent of `(x & y) == 0`,
                        // thus giving us `1` if zero and `0` if non-zero
                        //
                        // `vptestm(x, y)` does the equivalent of `(x & y) != 0`,
                        // thus giving us `1` if non-zero and `0` if zero
                        //
                        // Given the mask produced `m`, we then do `zf: (m == Zero)`, `cf: (m == AllBitsSet)`
                        //
                        // Thus, for either we can first emit `vptestm(x, y)`. This gives us a mask where
                        // `0` means the corresponding elements compared to zero. The subsequent `kortest`
                        // will then set `ZF: 1` if all elements were 0 and `ZF: 0` if any elements were
                        // non-zero. The default GenCondition then remain correct

                        assert(testIntrinsicId == NI_AVX512_PTESTM);

                        GenTree* nestedOp1 = op1Intrinsic->Op(1);
                        GenTree* nestedOp2 = op1Intrinsic->Op(2);

                        if (nestedOp2->isContained() && nestedOp2->OperIsHWIntrinsic())
                        {
                            GenTreeHWIntrinsic* nestedIntrin   = nestedOp2->AsHWIntrinsic();
                            NamedIntrinsic      nestedIntrinId = nestedIntrin->GetHWIntrinsicId();

                            if ((nestedIntrinId == NI_SSE42_MoveAndDuplicate) ||
                                (nestedIntrinId == NI_AVX2_BroadcastScalarToVector128) ||
                                (nestedIntrinId == NI_AVX2_BroadcastScalarToVector256) ||
                                (nestedIntrinId == NI_AVX512_BroadcastScalarToVector512))
                            {
                                // We need to rewrite the embedded broadcast back to a regular constant
                                // so that the subsequent containment check for ptestm can determine
                                // if the embedded broadcast is still relevant

                                GenTree* broadcastOp = nestedIntrin->Op(1);

                                if (broadcastOp->OperIsHWIntrinsic(NI_Vector128_CreateScalarUnsafe))
                                {
                                    BlockRange().Remove(broadcastOp);
                                    broadcastOp = broadcastOp->AsHWIntrinsic()->Op(1);
                                }

                                assert(broadcastOp->OperIsConst());

                                GenTree* vecCns =
                                    comp->gtNewSimdCreateBroadcastNode(simdType, broadcastOp,
                                                                       op1Intrinsic->GetSimdBaseJitType(), simdSize);

                                assert(vecCns->IsCnsVec());
                                BlockRange().InsertAfter(broadcastOp, vecCns);
                                nestedOp2 = vecCns;

                                BlockRange().Remove(broadcastOp);
                                BlockRange().Remove(nestedIntrin);
                            }
                        }

                        node->Op(1) = nestedOp1;
                        node->Op(2) = nestedOp2;

                        // Make sure we aren't contained since ptestm will do its own containment check
                        nestedOp2->ClearContained();

                        if (varTypeIsSmall(simdBaseType))
                        {
                            // Fixup the base type so embedded broadcast and the mask size checks still work

                            if (varTypeIsUnsigned(simdBaseType))
                            {
                                simdBaseJitType = CORINFO_TYPE_UINT;
                                simdBaseType    = TYP_UINT;
                            }
                            else
                            {
                                simdBaseJitType = CORINFO_TYPE_INT;
                                simdBaseType    = TYP_INT;
                            }
                            node->SetSimdBaseJitType(simdBaseJitType);

                            maskBaseJitType = simdBaseJitType;
                            maskBaseType    = simdBaseType;
                        }

                        BlockRange().Remove(op1);
                        BlockRange().Remove(op2);

                        skipReplaceOperands = true;
                        break;
                    }

                    default:
                    {
                        // We cannot optimize `AndNot` since `vptestnm` does ~(x & y)
                        break;
                    }
                }
            }

            if (!skipReplaceOperands)
            {
                node->Op(1) = op1;
                BlockRange().Remove(op2);

                LIR::Use op1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(op1Use);
                op1 = node->Op(1);

                op2 = comp->gtClone(op1);
                BlockRange().InsertAfter(op1, op2);
                node->Op(2) = op2;
            }

            node->gtType = TYP_MASK;
            node->ChangeHWIntrinsicId(testIntrinsicId);

            LowerNode(node);
            maskNode = node;
        }

        if (!maskNode->TypeIs(TYP_MASK))
        {
            assert(node == maskNode);

            // We're not consuming the underlying mask directly, even if one exists,
            // so ensure that we track the base type as the one we'll be producing
            // via the vector comparison introduced here.
            maskBaseJitType = simdBaseJitType;

            // We have `x == y` or `x != y` both of which where we want to find `AllBitsSet` in the mask since
            // we can directly do the relevant comparison. Given the above tables then when we have a full mask
            // we can simply check against `CF == 1` for `op_Equality` and `ZF == 0` for `op_Inequality`.
            //
            // For a partial mask, we need to invert the `op_Equality` comparisons which means that we now need
            // to check for `ZF == 1` (we're looking for `AllBitsSet`, that is `all match`). For `op_Inequality`
            // we can keep things as is since we're looking for `any match` and just want to check `ZF == 0`

            if (count < 8)
            {
                assert((count == 1) || (count == 2) || (count == 4));
                maskIntrinsicId = NI_AVX512_CompareNotEqualMask;
            }
            else
            {
                assert((count == 8) || (count == 16) || (count == 32) || (count == 64));

                if (cmpOp == GT_EQ)
                {
                    cmpCnd = GenCondition::C;
                }
                else
                {
                    maskIntrinsicId = NI_AVX512_CompareNotEqualMask;
                }
            }

            node->gtType = TYP_MASK;
            node->ChangeHWIntrinsicId(maskIntrinsicId);

            LowerNode(node);
            maskNode = node;
        }

        LIR::Use use;
        if (BlockRange().TryGetUse(maskNode, &use))
        {
            GenTreeHWIntrinsic* cc;

            cc = comp->gtNewSimdHWIntrinsicNode(simdType, maskNode, NI_AVX512_KORTEST, maskBaseJitType, simdSize);
            BlockRange().InsertBefore(nextNode, cc);

            use.ReplaceWith(cc);
            LowerHWIntrinsicCC(cc, NI_AVX512_KORTEST, cmpCnd);

            nextNode = cc->gtNext;
        }
        return nextNode;
    }

    assert(simdSize != 64);

    NamedIntrinsic cmpIntrinsic;
    CorInfoType    cmpJitType;
    NamedIntrinsic mskIntrinsic;
    CorInfoType    mskJitType;
    int            mskConstant;

    switch (simdBaseType)
    {
        case TYP_BYTE:
        case TYP_UBYTE:
        case TYP_SHORT:
        case TYP_USHORT:
        case TYP_INT:
        case TYP_UINT:
        {
            cmpJitType = simdBaseJitType;
            mskJitType = CORINFO_TYPE_UBYTE;

            if (simdSize == 32)
            {
                cmpIntrinsic = NI_AVX2_CompareEqual;
                mskIntrinsic = NI_AVX2_MoveMask;
                mskConstant  = -1;
            }
            else
            {
                assert(simdSize == 16);

                cmpIntrinsic = NI_X86Base_CompareEqual;
                mskIntrinsic = NI_X86Base_MoveMask;
                mskConstant  = 0xFFFF;
            }
            break;
        }

        case TYP_LONG:
        case TYP_ULONG:
        {
            mskJitType = CORINFO_TYPE_UBYTE;
            cmpJitType = simdBaseJitType;

            if (simdSize == 32)
            {
                cmpIntrinsic = NI_AVX2_CompareEqual;
                mskIntrinsic = NI_AVX2_MoveMask;
                mskConstant  = -1;
            }
            else
            {
                assert(simdSize == 16);

                if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                {
                    cmpIntrinsic = NI_SSE42_CompareEqual;
                }
                else
                {
                    cmpIntrinsic = NI_X86Base_CompareEqual;
                    cmpJitType   = CORINFO_TYPE_UINT;
                }
                mskIntrinsic = NI_X86Base_MoveMask;
                mskConstant  = 0xFFFF;
            }
            break;
        }

        case TYP_FLOAT:
        {
            cmpJitType = simdBaseJitType;
            mskJitType = simdBaseJitType;

            if (simdSize == 32)
            {
                cmpIntrinsic = NI_AVX_CompareEqual;
                mskIntrinsic = NI_AVX_MoveMask;
                mskConstant  = 0xFF;
            }
            else
            {
                cmpIntrinsic = NI_X86Base_CompareEqual;
                mskIntrinsic = NI_X86Base_MoveMask;

                if (simdSize == 16)
                {
                    mskConstant = 0xF;
                }
                else if (simdSize == 12)
                {
                    mskConstant = 0x7;
                }
                else
                {
                    assert(simdSize == 8);
                    mskConstant = 0x3;
                }
            }
            break;
        }

        case TYP_DOUBLE:
        {
            cmpJitType = simdBaseJitType;
            mskJitType = simdBaseJitType;

            if (simdSize == 32)
            {
                cmpIntrinsic = NI_AVX_CompareEqual;
                mskIntrinsic = NI_AVX_MoveMask;
                mskConstant  = 0xF;
            }
            else
            {
                assert(simdSize == 16);

                cmpIntrinsic = NI_X86Base_CompareEqual;
                mskIntrinsic = NI_X86Base_MoveMask;
                mskConstant  = 0x3;
            }
            break;
        }

        default:
        {
            unreached();
        }
    }

    GenTree* cmp = comp->gtNewSimdHWIntrinsicNode(simdType, op1, op2, cmpIntrinsic, cmpJitType, simdSize);
    BlockRange().InsertBefore(node, cmp);
    LowerNode(cmp);

    GenTree* msk = comp->gtNewSimdHWIntrinsicNode(TYP_INT, cmp, mskIntrinsic, mskJitType, simdSize);
    BlockRange().InsertAfter(cmp, msk);
    LowerNode(msk);

    GenTree* mskCns = comp->gtNewIconNode(mskConstant, TYP_INT);
    BlockRange().InsertAfter(msk, mskCns);

    if ((simdBaseType == TYP_FLOAT) && (simdSize < 16))
    {
        // For TYP_SIMD8 and TYP_SIMD12 we need to clear the upper bits and can't assume their value

        GenTree* tmp = comp->gtNewOperNode(GT_AND, TYP_INT, msk, mskCns);
        BlockRange().InsertAfter(mskCns, tmp);
        LowerNode(tmp);

        msk = tmp;

        mskCns = comp->gtNewIconNode(mskConstant, TYP_INT);
        BlockRange().InsertAfter(msk, mskCns);
    }

    node->ChangeOper(cmpOp);
    node->ChangeType(TYP_INT);
    node->AsOp()->gtOp1 = msk;
    node->AsOp()->gtOp2 = mskCns;

    GenTree* cc = LowerNodeCC(node, cmpCnd);

    node->gtType = TYP_VOID;
    node->ClearUnusedValue();

    return LowerNode(node);
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicCndSel: Lowers a Vector128 or Vector256 Conditional Select call
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsicCndSel(GenTreeHWIntrinsic* node)
{
    var_types   simdType        = node->gtType;
    CorInfoType simdBaseJitType = node->GetSimdBaseJitType();
    var_types   simdBaseType    = node->GetSimdBaseType();
    unsigned    simdSize        = node->GetSimdSize();

    assert(varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);

    // Get the three arguments to ConditionalSelect we stored in node
    // op1: the condition vector
    // op2: the left vector
    // op3: the right vector
    GenTree* op1 = node->Op(1);
    GenTree* op2 = node->Op(2);
    GenTree* op3 = node->Op(3);

    // If the condition vector comes from a hardware intrinsic that
    // returns a per-element mask, we can optimize the entire
    // conditional select to a single BlendVariable instruction
    // (if supported by the architecture)

    // First, determine if the condition is a per-element mask
    if (op1->IsVectorPerElementMask(simdBaseType, simdSize))
    {
        // Next, determine if the target architecture supports BlendVariable
        NamedIntrinsic blendVariableId = NI_Illegal;

        bool isOp1CvtMaskToVector = op1->OperIsConvertMaskToVector();

        if ((simdSize == 64) || isOp1CvtMaskToVector)
        {
            GenTree* maskNode;

            if (isOp1CvtMaskToVector)
            {
                GenTreeHWIntrinsic* cvtMaskToVector = op1->AsHWIntrinsic();

                maskNode = cvtMaskToVector->Op(1);
                BlockRange().Remove(op1);

                // We need to change the base type to match the underlying mask size to ensure
                // the right instruction variant is picked. If the CndSel was for TYP_INT but
                // the mask was for TYP_DOUBLE then we'd generate vpblendmd when we really want
                // vpblendmq. Changing the size is fine since CndSel itself is bitwise and the
                // the mask is just representing entire elements at a given size.

                CorInfoType maskBaseJitType = cvtMaskToVector->GetSimdBaseJitType();
                node->SetSimdBaseJitType(maskBaseJitType);
            }
            else
            {
                maskNode = comp->gtNewSimdCvtVectorToMaskNode(TYP_MASK, op1, simdBaseJitType, simdSize);
                BlockRange().InsertBefore(node, maskNode);
            }

            assert(maskNode->TypeIs(TYP_MASK));
            blendVariableId = NI_AVX512_BlendVariableMask;
            op1             = maskNode;
        }
        else if (op2->IsVectorZero() || op3->IsVectorZero())
        {
            // If either of the value operands is const zero, we can optimize down to AND or AND_NOT.
            GenTree* binOp = nullptr;

            if (op3->IsVectorZero())
            {
                binOp = comp->gtNewSimdBinOpNode(GT_AND, simdType, op1, op2, simdBaseJitType, simdSize);
                BlockRange().Remove(op3);
            }
            else
            {
                binOp = comp->gtNewSimdBinOpNode(GT_AND_NOT, simdType, op3, op1, simdBaseJitType, simdSize);
                BlockRange().Remove(op2);
            }

            BlockRange().InsertAfter(node, binOp);

            LIR::Use use;
            if (BlockRange().TryGetUse(node, &use))
            {
                use.ReplaceWith(binOp);
            }
            else
            {
                binOp->SetUnusedValue();
            }

            BlockRange().Remove(node);
            return LowerNode(binOp);
        }
        else if (simdSize == 32)
        {
            // For Vector256 (simdSize == 32), BlendVariable for floats/doubles
            // is available on AVX, whereas other types (integrals) require AVX2

            if (varTypeIsFloating(simdBaseType))
            {
                // This should have already been confirmed
                assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));
                blendVariableId = NI_AVX_BlendVariable;
            }
            else if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
            {
                blendVariableId = NI_AVX2_BlendVariable;
            }
        }
        else if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
        {
            // For Vector128, BlendVariable is available on SSE41
            blendVariableId = NI_SSE42_BlendVariable;
        }

        // If blendVariableId has been set, the architecture supports BlendVariable, so we can optimize
        if (blendVariableId != NI_Illegal)
        {
            // result = BlendVariable op3 (right) op2 (left) op1 (mask)
            node->ResetHWIntrinsicId(blendVariableId, comp, op3, op2, op1);
            return LowerNode(node);
        }
    }

    if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX512))
    {
        // We can't use the mask, but we can emit a ternary logic node
        NamedIntrinsic ternaryLogicId = NI_AVX512_TernaryLogic;

        GenTree* control = comp->gtNewIconNode(0xCA); // (B & A) | (C & ~A)
        BlockRange().InsertBefore(node, control);

        node->ResetHWIntrinsicId(ternaryLogicId, comp, op1, op2, op3, control);
        return LowerNode(node);
    }

    // We cannot optimize, so produce unoptimized instructions
    assert(simdSize != 64);

    // We will be constructing the following parts:
    //          /--*  op1 simd16
    //          *  STORE_LCL_VAR simd16
    //   op1  =    LCL_VAR       simd16
    //   tmp1 =    LCL_VAR       simd16
    //   ...

    GenTree* tmp1;
    GenTree* tmp2;
    GenTree* tmp3;
    GenTree* tmp4;

    LIR::Use op1Use(BlockRange(), &node->Op(1), node);
    ReplaceWithLclVar(op1Use);
    op1 = node->Op(1);

    tmp1 = comp->gtClone(op1);
    BlockRange().InsertAfter(op1, tmp1);

    // ...
    // tmp2 = op1 & op2
    // ...
    tmp2 = comp->gtNewSimdBinOpNode(GT_AND, simdType, op1, op2, simdBaseJitType, simdSize);
    BlockRange().InsertAfter(op2, tmp2);
    LowerNode(tmp2);

    // ...
    // tmp3 = op3 & ~tmp1
    // ...
    tmp3 = comp->gtNewSimdBinOpNode(GT_AND_NOT, simdType, op3, tmp1, simdBaseJitType, simdSize);
    BlockRange().InsertAfter(op3, tmp3);
    LowerNode(tmp3);

    // ...
    // tmp4 = tmp2 | tmp3
    // ...
    tmp4 = comp->gtNewSimdBinOpNode(GT_OR, simdType, tmp2, tmp3, simdBaseJitType, simdSize);
    BlockRange().InsertBefore(node, tmp4);

    LIR::Use use;
    if (BlockRange().TryGetUse(node, &use))
    {
        use.ReplaceWith(tmp4);
    }
    else
    {
        tmp4->SetUnusedValue();
    }

    BlockRange().Remove(node);
    return LowerNode(tmp4);
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicTernaryLogic: Lowers an AVX512 TernaryLogic call
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsicTernaryLogic(GenTreeHWIntrinsic* node)
{
    assert(comp->canUseEvexEncodingDebugOnly());

    // These are the control bytes used for TernaryLogic

    const uint8_t A = 0xF0;
    const uint8_t B = 0xCC;
    const uint8_t C = 0xAA;

    var_types   simdType        = node->gtType;
    CorInfoType simdBaseJitType = node->GetSimdBaseJitType();
    var_types   simdBaseType    = node->GetSimdBaseType();
    unsigned    simdSize        = node->GetSimdSize();

    assert(varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);

    GenTree* op1 = node->Op(1);
    GenTree* op2 = node->Op(2);
    GenTree* op3 = node->Op(3);
    GenTree* op4 = node->Op(4);

    if (op4->IsCnsIntOrI())
    {
        uint8_t                 control  = static_cast<uint8_t>(op4->AsIntConCommon()->IconValue());
        const TernaryLogicInfo& info     = TernaryLogicInfo::lookup(control);
        TernaryLogicUseFlags    useFlags = info.GetAllUseFlags();

        switch (control)
        {
            case static_cast<uint8_t>((C & A) | (B & ~A)): // A ? C : B
            case static_cast<uint8_t>((C & B) | (A & ~B)): // B ? C : A
            case static_cast<uint8_t>((B & C) | (A & ~C)): // C ? B : A
            case static_cast<uint8_t>((B & A) | (C & ~A)): // A ? B : C
            case static_cast<uint8_t>((A & B) | (C & ~B)): // B ? A : C
            case static_cast<uint8_t>((A & C) | (B & ~C)): // C ? A : B
            {
                // For the operations that work as a conditional select, we want
                // to try and optimize it to use BlendVariableMask when the condition
                // is already a TYP_MASK

                assert(info.oper1 == TernaryLogicOperKind::Select);
                assert(info.oper2 == TernaryLogicOperKind::Select);
                assert(info.oper3 == TernaryLogicOperKind::Cond);

                GenTree* condition   = nullptr;
                GenTree* selectTrue  = nullptr;
                GenTree* selectFalse = nullptr;

                if (info.oper1Use == TernaryLogicUseFlags::A)
                {
                    selectTrue = op1;

                    if (info.oper2Use == TernaryLogicUseFlags::B)
                    {
                        assert(info.oper3Use == TernaryLogicUseFlags::C);

                        selectFalse = op2;
                        condition   = op3;
                    }
                    else
                    {
                        assert(info.oper2Use == TernaryLogicUseFlags::C);
                        assert(info.oper3Use == TernaryLogicUseFlags::B);

                        selectFalse = op3;
                        condition   = op2;
                    }
                }
                else if (info.oper1Use == TernaryLogicUseFlags::B)
                {
                    selectTrue = op2;

                    if (info.oper2Use == TernaryLogicUseFlags::A)
                    {
                        assert(info.oper3Use == TernaryLogicUseFlags::C);

                        selectFalse = op1;
                        condition   = op3;
                    }
                    else
                    {
                        assert(info.oper2Use == TernaryLogicUseFlags::C);
                        assert(info.oper3Use == TernaryLogicUseFlags::A);

                        selectFalse = op3;
                        condition   = op1;
                    }
                }
                else
                {
                    assert(info.oper1Use == TernaryLogicUseFlags::C);

                    selectTrue = op3;

                    if (info.oper2Use == TernaryLogicUseFlags::A)
                    {
                        assert(info.oper3Use == TernaryLogicUseFlags::B);

                        selectFalse = op1;
                        condition   = op2;
                    }
                    else
                    {
                        assert(info.oper2Use == TernaryLogicUseFlags::B);
                        assert(info.oper3Use == TernaryLogicUseFlags::A);

                        selectFalse = op2;
                        condition   = op1;
                    }
                }

                if (condition->OperIsConvertMaskToVector())
                {
                    GenTree* tmp = condition->AsHWIntrinsic()->Op(1);
                    if (tmp->OperIsHWIntrinsic())
                    {
                        BlockRange().Remove(condition);
                        condition = tmp;
                    }
                    else
                    {
                        // We can't change to a BlendVariable intrinsic
                        break;
                    }
                }
                else if (!varTypeIsMask(condition))
                {
                    if (!condition->OperIsHWIntrinsic())
                    {
                        break;
                    }

                    GenTreeHWIntrinsic* cndNode = condition->AsHWIntrinsic();
                    NamedIntrinsic      cndId   = cndNode->GetHWIntrinsicId();

                    switch (cndId)
                    {
                        case NI_AVX_Compare:
                        {
                            cndId = NI_AVX512_CompareMask;
                            break;
                        }

                        case NI_X86Base_CompareEqual:
                        case NI_SSE42_CompareEqual:
                        case NI_AVX_CompareEqual:
                        case NI_AVX2_CompareEqual:
                        {
                            cndId = NI_AVX512_CompareEqualMask;
                            break;
                        }

                        case NI_X86Base_CompareGreaterThan:
                        case NI_SSE42_CompareGreaterThan:
                        case NI_AVX_CompareGreaterThan:
                        case NI_AVX2_CompareGreaterThan:
                        {
                            cndId = NI_AVX512_CompareGreaterThanMask;
                            break;
                        }

                        case NI_X86Base_CompareGreaterThanOrEqual:
                        case NI_AVX_CompareGreaterThanOrEqual:
                        {
                            cndId = NI_AVX512_CompareGreaterThanOrEqualMask;
                            break;
                        }

                        case NI_X86Base_CompareLessThan:
                        case NI_SSE42_CompareLessThan:
                        case NI_AVX_CompareLessThan:
                        case NI_AVX2_CompareLessThan:
                        {
                            cndId = NI_AVX512_CompareLessThanMask;
                            break;
                        }

                        case NI_X86Base_CompareLessThanOrEqual:
                        case NI_AVX_CompareLessThanOrEqual:
                        {
                            cndId = NI_AVX512_CompareLessThanOrEqualMask;
                            break;
                        }

                        case NI_X86Base_CompareNotEqual:
                        case NI_AVX_CompareNotEqual:
                        {
                            cndId = NI_AVX512_CompareNotEqualMask;
                            break;
                        }

                        case NI_X86Base_CompareNotGreaterThan:
                        case NI_AVX_CompareNotGreaterThan:
                        {
                            cndId = NI_AVX512_CompareGreaterThanMask;
                            break;
                        }

                        case NI_X86Base_CompareNotGreaterThanOrEqual:
                        case NI_AVX_CompareNotGreaterThanOrEqual:
                        {
                            cndId = NI_AVX512_CompareNotGreaterThanOrEqualMask;
                            break;
                        }

                        case NI_X86Base_CompareNotLessThan:
                        case NI_AVX_CompareNotLessThan:
                        {
                            cndId = NI_AVX512_CompareNotLessThanMask;
                            break;
                        }

                        case NI_X86Base_CompareNotLessThanOrEqual:
                        case NI_AVX_CompareNotLessThanOrEqual:
                        {
                            cndId = NI_AVX512_CompareNotLessThanOrEqualMask;
                            break;
                        }

                        case NI_X86Base_CompareOrdered:
                        case NI_AVX_CompareOrdered:
                        {
                            cndId = NI_AVX512_CompareOrderedMask;
                            break;
                        }

                        case NI_X86Base_CompareUnordered:
                        case NI_AVX_CompareUnordered:
                        {
                            cndId = NI_AVX512_CompareUnorderedMask;
                            break;
                        }

                        default:
                        {
                            assert(!HWIntrinsicInfo::ReturnsPerElementMask(cndId));
                            cndId = NI_Illegal;
                            break;
                        }
                    }

                    if (cndId == NI_Illegal)
                    {
                        break;
                    }

                    cndNode->gtType = TYP_MASK;
                    cndNode->ChangeHWIntrinsicId(cndId);
                }

                assert(varTypeIsMask(condition));

                // The TernaryLogic node normalizes small SIMD base types on import. To optimize
                // to BlendVariableMask, we need to "un-normalize". We no longer have the original
                // base type, so we use the mask base type instead.
                NamedIntrinsic intrinsicId = node->GetHWIntrinsicId();

                if (!condition->OperIsHWIntrinsic())
                {
                    break;
                }

                node->SetSimdBaseJitType(condition->AsHWIntrinsic()->GetSimdBaseJitType());

                node->ResetHWIntrinsicId(NI_AVX512_BlendVariableMask, comp, selectFalse, selectTrue, condition);
                BlockRange().Remove(op4);
                break;
            }

            default:
            {
                switch (useFlags)
                {
                    case TernaryLogicUseFlags::A:
                    {
                        // Swap the operands here to make the containment checks in codegen significantly simpler
                        std::swap(node->Op(1), node->Op(3));

                        // Make sure we also fixup the control byte
                        control = TernaryLogicInfo::GetTernaryControlByte(info, C, B, A);
                        op4->AsIntCon()->SetIconValue(control);

                        useFlags = TernaryLogicUseFlags::C;
                        break;
                    }

                    case TernaryLogicUseFlags::B:
                    {
                        // Swap the operands here to make the containment checks in codegen significantly simpler
                        std::swap(node->Op(2), node->Op(3));

                        // Make sure we also fixup the control byte
                        control = TernaryLogicInfo::GetTernaryControlByte(info, A, C, B);
                        op4->AsIntCon()->SetIconValue(control);

                        useFlags = TernaryLogicUseFlags::C;
                        break;
                    }

                    case TernaryLogicUseFlags::AB:
                    {
                        // Swap the operands here to make the containment checks in codegen significantly simpler
                        std::swap(node->Op(2), node->Op(3));
                        std::swap(node->Op(1), node->Op(2));

                        // Make sure we also fixup the control byte
                        control = TernaryLogicInfo::GetTernaryControlByte(info, B, C, A);
                        op4->AsIntCon()->SetIconValue(control);

                        useFlags = TernaryLogicUseFlags::BC;
                        break;
                    }

                    case TernaryLogicUseFlags::AC:
                    {
                        // Swap the operands here to make the containment checks in codegen significantly simpler
                        std::swap(node->Op(1), node->Op(2));

                        // Make sure we also fixup the control byte
                        control = TernaryLogicInfo::GetTernaryControlByte(info, B, A, C);
                        op4->AsIntCon()->SetIconValue(control);

                        useFlags = TernaryLogicUseFlags::BC;
                        break;
                    }

                    default:
                    {
                        break;
                    }
                }

                // Update the locals to reflect any operand swaps we did above.

                op1 = node->Op(1);
                op2 = node->Op(2);
                op3 = node->Op(3);
                assert(op4 == node->Op(4));

                GenTree* replacementNode = nullptr;

                switch (useFlags)
                {
                    case TernaryLogicUseFlags::None:
                    {
                        // Encountering none should be very rare and so we'll handle
                        // it, but we won't try to optimize it by finding an existing
                        // constant to reuse or similar, as that's more expensive

                        op1->SetUnusedValue();
                        op2->SetUnusedValue();
                        op3->SetUnusedValue();

                        if (control == 0x00)
                        {
                            replacementNode = comp->gtNewZeroConNode(simdType);
                        }
                        else
                        {
                            assert(control == 0xFF);
                            replacementNode = comp->gtNewAllBitsSetConNode(simdType);
                        }

                        BlockRange().InsertBefore(node, replacementNode);
                        break;
                    }

                    case TernaryLogicUseFlags::C:
                    {
                        // Encountering `select(c)` instead of `not(c)` should likewise
                        // be rare, but we'll handle it in case the combined operations
                        // are just right to cause it to appear

                        if (control == C)
                        {
                            op1->SetUnusedValue();
                            op2->SetUnusedValue();

                            replacementNode = op3;
                            break;
                        }

                        // For not, we do want to check if we already have reusable constants as
                        // this can occur for the normal lowering pattern around `xor(c, AllBitsSet)`

                        if (!op1->IsCnsVec())
                        {
                            op1->SetUnusedValue();
                            op1 = comp->gtNewZeroConNode(simdType);

                            BlockRange().InsertBefore(node, op1);
                            node->Op(1) = op1;
                        }

                        if (!op2->IsCnsVec())
                        {
                            op2->SetUnusedValue();
                            op2 = comp->gtNewZeroConNode(simdType);

                            BlockRange().InsertBefore(node, op2);
                            node->Op(2) = op2;
                        }
                        break;
                    }

                    case TernaryLogicUseFlags::BC:
                    {
                        if (!op1->IsCnsVec())
                        {
                            op1->SetUnusedValue();
                            op1 = comp->gtNewZeroConNode(simdType);

                            BlockRange().InsertBefore(node, op1);
                            node->Op(1) = op1;
                        }
                        break;
                    }

                    default:
                    {
                        assert(useFlags == TernaryLogicUseFlags::ABC);
                        break;
                    }
                }

                if (replacementNode != nullptr)
                {
                    LIR::Use use;
                    if (BlockRange().TryGetUse(node, &use))
                    {
                        use.ReplaceWith(replacementNode);
                    }
                    else
                    {
                        replacementNode->SetUnusedValue();
                    }

                    GenTree* next = node->gtNext;
                    BlockRange().Remove(op4);
                    BlockRange().Remove(node);
                    return next;
                }
                break;
            }
        }
    }

    // TODO-XARCH-AVX512: We should look for nested TernaryLogic and BitwiseOper
    // nodes so that we can fully take advantage of the instruction where possible

    ContainCheckHWIntrinsic(node);
    return node->gtNext;
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicCreate: Lowers a Vector128 or Vector256 or Vector512 Create call
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsicCreate(GenTreeHWIntrinsic* node)
{
    NamedIntrinsic intrinsicId     = node->GetHWIntrinsicId();
    var_types      simdType        = node->gtType;
    CorInfoType    simdBaseJitType = node->GetSimdBaseJitType();
    var_types      simdBaseType    = node->GetSimdBaseType();
    unsigned       simdSize        = node->GetSimdSize();
    simd_t         simdVal         = {};

    if ((simdSize == 8) && (simdType == TYP_DOUBLE))
    {
        // TODO-Cleanup: Struct retyping means we have the wrong type here. We need to
        //               manually fix it up so the simdType checks below are correct.
        simdType = TYP_SIMD8;
    }

    assert(varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);

    GenTree* op1 = node->Op(1);

    // Spare GenTrees to be used for the lowering logic below
    // Defined upfront to avoid naming conflicts, etc...
    GenTree* idx  = nullptr;
    GenTree* tmp1 = nullptr;
    GenTree* tmp2 = nullptr;
    GenTree* tmp3 = nullptr;

    bool   isConstant     = GenTreeVecCon::IsHWIntrinsicCreateConstant<simd_t>(node, simdVal);
    bool   isCreateScalar = HWIntrinsicInfo::IsVectorCreateScalar(intrinsicId);
    size_t argCnt         = node->GetOperandCount();

    if (isConstant)
    {
        assert((simdSize == 8) || (simdSize == 12) || (simdSize == 16) || (simdSize == 32) || (simdSize == 64));

        for (GenTree* arg : node->Operands())
        {
#if !defined(TARGET_64BIT)
            if (arg->OperIsLong())
            {
                BlockRange().Remove(arg->gtGetOp1());
                BlockRange().Remove(arg->gtGetOp2());
            }
#endif // !TARGET_64BIT
            BlockRange().Remove(arg);
        }

        GenTreeVecCon* vecCon = comp->gtNewVconNode(simdType);
        memcpy(&vecCon->gtSimdVal, &simdVal, simdSize);
        BlockRange().InsertBefore(node, vecCon);

        LIR::Use use;
        if (BlockRange().TryGetUse(node, &use))
        {
            use.ReplaceWith(vecCon);
        }
        else
        {
            vecCon->SetUnusedValue();
        }

        BlockRange().Remove(node);

        return vecCon->gtNext;
    }
    else if (argCnt == 1)
    {
        if (isCreateScalar)
        {
            switch (simdBaseType)
            {
                case TYP_BYTE:
                case TYP_UBYTE:
                case TYP_SHORT:
                case TYP_USHORT:
                {
                    // The smallest scalar SIMD load that zeroes upper elements is 32 bits, so for CreateScalar,
                    // we must ensure that the upper bits of that 32-bit value are zero if the base type is small.
                    //
                    // The most likely case is that op1 is a cast from int/long to the base type:
                    // *  CAST      int <- short <- int/long
                    // If the base type is signed, that cast will be sign-extending, but we need zero extension,
                    // so we may be able to simply retype the cast to the unsigned type of the same size.
                    // This is valid only if the cast is not checking overflow and is not containing a load.
                    //
                    // It's also possible we have a memory load of the base type:
                    // *  IND       short
                    // We can likewise change the type of the indir to force zero extension on load.
                    //
                    // If we can't safely retype one of the above patterns and don't already have a cast to the
                    // correct unsigned type, we will insert our own cast.

                    node->SetSimdBaseJitType(CORINFO_TYPE_INT);

                    var_types unsignedType = varTypeToUnsigned(simdBaseType);

                    if (op1->OperIs(GT_CAST) && !op1->gtOverflow() && !op1->AsCast()->CastOp()->isContained() &&
                        (genTypeSize(op1->CastToType()) == genTypeSize(simdBaseType)))
                    {
                        op1->AsCast()->gtCastType = unsignedType;
                    }
                    else if (op1->OperIs(GT_IND, GT_LCL_FLD) && (genTypeSize(op1) == genTypeSize(simdBaseType)))
                    {
                        op1->gtType = unsignedType;
                    }
                    else if (!op1->OperIs(GT_CAST) || (op1->AsCast()->CastToType() != unsignedType))
                    {
                        tmp1        = comp->gtNewCastNode(TYP_INT, op1, /* fromUnsigned */ false, unsignedType);
                        node->Op(1) = tmp1;
                        BlockRange().InsertAfter(op1, tmp1);
                        LowerNode(tmp1);
                    }

                    break;
                }

                default:
                {
                    break;
                }
            }

            ContainCheckHWIntrinsic(node);
            return node->gtNext;
        }

        // We have the following (where simd is simd16, simd32 or simd64):
        //          /--*  op1  T
        //   node = *  HWINTRINSIC   simd   T Create

        if (intrinsicId == NI_Vector512_Create)
        {
            assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX512));
            // We will be constructing the following parts:
            //          /--*  op1  T
            //   tmp1 = *  HWINTRINSIC   simd32 T CreateScalarUnsafe
            //          /--*  tmp1 simd16
            //   node = *  HWINTRINSIC   simd64 T BroadcastScalarToVector512

            // This is roughly the following managed code:
            //   var tmp1 = Vector256.CreateScalarUnsafe(op1);
            //   return Avx512.BroadcastScalarToVector512(tmp1);

            tmp1 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, op1, simdBaseJitType, 16);
            LowerNode(tmp1);
            node->ResetHWIntrinsicId(NI_AVX512_BroadcastScalarToVector512, tmp1);
            return LowerNode(node);
        }

        // We have the following (where simd is simd16 or simd32):
        //          /--*  op1  T
        //   node = *  HWINTRINSIC   simd   T Create

        if (intrinsicId == NI_Vector256_Create)
        {
            if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
            {
                // We will be constructing the following parts:
                //          /--*  op1  T
                //   tmp1 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
                //          /--*  tmp1 simd16
                //   node = *  HWINTRINSIC   simd32 T BroadcastScalarToVector256

                // This is roughly the following managed code:
                //   var tmp1 = Vector128.CreateScalarUnsafe(op1);
                //   return Avx2.BroadcastScalarToVector256(tmp1);

                tmp1 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, op1, simdBaseJitType, 16);
                LowerNode(tmp1);

                node->ResetHWIntrinsicId(NI_AVX2_BroadcastScalarToVector256, tmp1);
                return LowerNode(node);
            }

            assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));

            // We will be constructing the following parts:
            //          /--*  op1  T
            //   tmp1 = *  HWINTRINSIC   simd16 T Create
            //          /--*  tmp1 simd16
            //          *  STORE_LCL_VAR simd16
            //   tmp1 =    LCL_VAR       simd16
            //   tmp2 =    LCL_VAR       simd16
            //          /--*  tmp2 simd16
            //   tmp3 = *  HWINTRINSIC   simd16 T ToVector256Unsafe
            //   idx  =    CNS_INT       int    0
            //          /--*  tmp3 simd32
            //          +--*  tmp1 simd16
            //   node = *  HWINTRINSIC simd32 T WithUpper

            // This is roughly the following managed code:
            //   var tmp1 = Vector128.Create(op1);
            //   var tmp2 = tmp1;
            //   var tmp3 = tmp2.ToVector256Unsafe();
            //   return tmp3.WithUpper(tmp1);

            tmp1 = comp->gtNewSimdCreateBroadcastNode(TYP_SIMD16, op1, simdBaseJitType, 16);
            BlockRange().InsertAfter(op1, tmp1);

            node->Op(1) = tmp1;
            LowerNode(tmp1);

            LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
            ReplaceWithLclVar(tmp1Use);
            tmp1 = node->Op(1);

            tmp2 = comp->gtClone(tmp1);
            BlockRange().InsertAfter(tmp1, tmp2);

            tmp3 =
                comp->gtNewSimdHWIntrinsicNode(TYP_SIMD32, tmp2, NI_Vector128_ToVector256Unsafe, simdBaseJitType, 16);
            BlockRange().InsertAfter(tmp2, tmp3);

            node->ResetHWIntrinsicId(NI_Vector256_WithUpper, comp, tmp3, tmp1);
            LowerNode(tmp3);

            return LowerNode(node);
        }

        assert(intrinsicId == NI_Vector128_Create);

        // We will be constructing the following parts:
        //          /--*  op1  T
        //   tmp1 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
        //   ...

        // This is roughly the following managed code:
        //   var tmp1 = Vector128.CreateScalarUnsafe(op1);
        //   ...

        tmp1 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, op1, simdBaseJitType, 16);
        LowerNode(tmp1);

        if ((simdBaseJitType != CORINFO_TYPE_DOUBLE) && comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
        {
            // We will be constructing the following parts:
            //   ...
            //           /--*  tmp1 simd16
            //   node  = *  HWINTRINSIC   simd16 T BroadcastScalarToVector128

            // This is roughly the following managed code:
            //   ...
            //   return Avx2.BroadcastScalarToVector128(tmp1);

            node->ChangeHWIntrinsicId(NI_AVX2_BroadcastScalarToVector128, tmp1);
            return LowerNode(node);
        }

        switch (simdBaseType)
        {
            case TYP_BYTE:
            case TYP_UBYTE:
            {
                if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                {
                    // We will be constructing the following parts:
                    //   ...
                    //   tmp2 =    CNS_VEC   simd16 0
                    //         /--*  tmp1 simd16
                    //         +--*  tmp2 simd16
                    //   node = *  HWINTRINSIC   simd16 ubyte Shuffle

                    // This is roughly the following managed code:
                    //   ...
                    //   var tmp2 = Vector128<byte>.Zero;
                    //   return Ssse3.Shuffle(tmp1, tmp2);

                    tmp2 = comp->gtNewZeroConNode(simdType);
                    BlockRange().InsertAfter(tmp1, tmp2);
                    LowerNode(tmp2);

                    node->ResetHWIntrinsicId(NI_SSE42_Shuffle, tmp1, tmp2);
                    break;
                }

                // We will be constructing the following parts:
                //   ...
                //          /--*  tmp1 simd16
                //          *  STORE_LCL_VAR simd16
                //   tmp1 =    LCL_VAR       simd16
                //   tmp2 =    LCL_VAR       simd16
                //          /--*  tmp1 simd16
                //          +--*  tmp2 simd16
                //   tmp1 = *  HWINTRINSIC   simd16 ubyte UnpackLow
                //   ...

                // This is roughly the following managed code:
                //   ...
                //   var tmp2 = tmp1;
                //   tmp1 = Sse2.UnpackLow(tmp1, tmp2);
                //   ...

                node->Op(1) = tmp1;
                LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(tmp1Use);
                tmp1 = node->Op(1);

                tmp2 = comp->gtClone(tmp1);
                BlockRange().InsertAfter(tmp1, tmp2);

                tmp1 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp1, tmp2, NI_X86Base_UnpackLow, CORINFO_TYPE_UBYTE,
                                                      simdSize);
                BlockRange().InsertAfter(tmp2, tmp1);
                LowerNode(tmp1);

                FALLTHROUGH;
            }

            case TYP_SHORT:
            case TYP_USHORT:
            {
                // We will be constructing the following parts:
                //   ...
                //          /--*  tmp1 simd16
                //          *  STORE_LCL_VAR simd16
                //   tmp1 =    LCL_VAR       simd16
                //   tmp2 =    LCL_VAR       simd16
                //          /--*  tmp1 simd16
                //          +--*  tmp2 simd16
                //   tmp1 = *  HWINTRINSIC   simd16 ushort UnpackLow
                //   ...

                // This is roughly the following managed code:
                //   ...
                //   var tmp2 = tmp1;
                //   tmp1 = Sse2.UnpackLow(tmp1, tmp2);
                //   ...

                node->Op(1) = tmp1;
                LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(tmp1Use);
                tmp1 = node->Op(1);

                tmp2 = comp->gtClone(tmp1);
                BlockRange().InsertAfter(tmp1, tmp2);

                tmp1 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp1, tmp2, NI_X86Base_UnpackLow, CORINFO_TYPE_USHORT,
                                                      simdSize);
                BlockRange().InsertAfter(tmp2, tmp1);
                LowerNode(tmp1);

                FALLTHROUGH;
            }

            case TYP_INT:
            case TYP_UINT:
            {
                // We will be constructing the following parts:
                //   ...
                //   idx  =    CNS_INT       int    0
                //          /--*  tmp1 simd16
                //          +--*  idx  int
                //   node = *  HWINTRINSIC   simd16 uint Shuffle

                // This is roughly the following managed code:
                //   ...
                //   return Sse2.Shuffle(tmp1, 0x00);

                idx = comp->gtNewIconNode(0x00, TYP_INT);
                BlockRange().InsertAfter(tmp1, idx);

                node->ResetHWIntrinsicId(NI_X86Base_Shuffle, tmp1, idx);
                node->SetSimdBaseJitType(CORINFO_TYPE_UINT);
                break;
            }

            case TYP_FLOAT:
            {
                if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX))
                {
                    // We will be constructing the following parts:
                    //   ...
                    //   idx  =    CNS_INT       int    0
                    //          /--*  tmp1 simd16
                    //          +--*  idx  int
                    //   node = *  HWINTRINSIC   simd16 float Permute

                    // This is roughly the following managed code:
                    //   ...
                    //   return Avx.Permute(tmp1, 0x00);

                    idx = comp->gtNewIconNode(0x00, TYP_INT);
                    BlockRange().InsertAfter(tmp1, idx);

                    node->ResetHWIntrinsicId(NI_AVX_Permute, tmp1, idx);
                    break;
                }

                // We will be constructing the following parts:
                //   ...
                //          /--*  tmp1 simd16
                //          *  STORE_LCL_VAR simd16
                //   tmp1 =    LCL_VAR       simd16
                //   tmp2 =    LCL_VAR       simd16
                //   idx  =    CNS_INT       int    0
                //          /--*  tmp1 simd16
                //          +--*  tmp2 simd16
                //          +--*  idx  int
                //   node = *  HWINTRINSIC   simd16 float Shuffle

                // This is roughly the following managed code:
                //   ...
                //   var tmp2 = tmp1;
                //   return Sse.Shuffle(tmp1, tmp2, 0x00);

                node->Op(1) = tmp1;
                LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(tmp1Use);
                tmp1 = node->Op(1);

                tmp2 = comp->gtClone(tmp1);
                BlockRange().InsertAfter(tmp1, tmp2);

                idx = comp->gtNewIconNode(0x00, TYP_INT);
                BlockRange().InsertAfter(tmp2, idx);

                node->ResetHWIntrinsicId(NI_X86Base_Shuffle, comp, tmp1, tmp2, idx);
                break;
            }

            case TYP_LONG:
            case TYP_ULONG:
            case TYP_DOUBLE:
            {
                if ((IsContainableMemoryOp(op1) || simdBaseType == TYP_DOUBLE) &&
                    comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                {
                    // We will be constructing the following parts:
                    //   ...
                    //          /--*  tmp1 simd16
                    //   node = *  HWINTRINSIC   simd16 double MoveAndDuplicate

                    // This is roughly the following managed code:
                    //   ...
                    //   return Sse3.MoveAndDuplicate(tmp1);

                    node->ChangeHWIntrinsicId(NI_SSE42_MoveAndDuplicate, tmp1);
                    node->SetSimdBaseJitType(CORINFO_TYPE_DOUBLE);
                    break;
                }

                // We will be constructing the following parts:
                //   ...
                //          /--*  tmp1 simd16
                //          *  STORE_LCL_VAR simd16
                //   tmp1 =    LCL_VAR       simd16
                //   tmp2 =    LCL_VAR       simd16
                //          /--*  tmp1 simd16
                //          +--*  tmp2 simd16
                //   node = *  HWINTRINSIC   simd16 T UnpackLow

                // This is roughly the following managed code:
                //   ...
                //   var tmp2 = tmp1;
                //   return Sse2.UnpackLow(tmp1, tmp2);

                node->Op(1) = tmp1;
                LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(tmp1Use);
                tmp1 = node->Op(1);

                tmp2 = comp->gtClone(tmp1);
                BlockRange().InsertAfter(tmp1, tmp2);

                node->ResetHWIntrinsicId(NI_X86Base_UnpackLow, tmp1, tmp2);
                break;
            }

            default:
            {
                unreached();
            }
        }

        return LowerNode(node);
    }

    if (intrinsicId == NI_Vector512_Create || intrinsicId == NI_Vector256_Create)
    {
        assert(argCnt >= (simdSize / genTypeSize(TYP_LONG)));
        assert(((simdSize == 64) && comp->compIsaSupportedDebugOnly(InstructionSet_AVX512)) ||
               ((simdSize == 32) && comp->compIsaSupportedDebugOnly(InstructionSet_AVX)));

        // The larger vector implementation is simplified by splitting the
        // job in half and delegating to the next smaller vector size.
        //
        // For example, for Vector512, we construct the following:
        //          /--*  op1 T
        //          +--*  ... T
        //   lo   = *  HWINTRINSIC   simd32 T Create
        //          /--*  ... T
        //          +--*  opN T
        //   hi   = *  HWINTRINSIC   simd32 T Create
        //          /--*  lo   simd64
        //          +--*  hi   simd32
        //   node = *  HWINTRINSIC   simd64 T WithUpper

        // This is roughly the following managed code:
        //   ...
        //   var lo   = Vector256.Create(op1, ...);
        //   var hi   = Vector256.Create(..., opN);
        //   return lo.WithUpper(hi);

        // Each Vector256.Create call gets half the operands. That is:
        //   lo = Vector256.Create(op1, op2);
        //   hi = Vector256.Create(op3, op4);
        // -or-
        //   lo = Vector256.Create(op1,  ..., op4);
        //   hi = Vector256.Create(op5,  ..., op8);
        // -or-
        //   lo = Vector256.Create(op1,  ..., op8);
        //   hi = Vector256.Create(op9,  ..., op16);
        // -or-
        //   lo = Vector256.Create(op1,  ..., op16);
        //   hi = Vector256.Create(op17, ..., op32);

        var_types      halfType   = comp->getSIMDTypeForSize(simdSize / 2);
        NamedIntrinsic halfCreate = (simdSize == 64) ? NI_Vector256_Create : NI_Vector128_Create;
        NamedIntrinsic withUpper  = (simdSize == 64) ? NI_Vector512_WithUpper : NI_Vector256_WithUpper;

        size_t halfArgCnt = argCnt / 2;
        assert((halfArgCnt * 2) == argCnt);

        GenTree* loInsertionPoint = LIR::LastNode(node->GetOperandArray(), halfArgCnt);
        GenTree* hiInsertionPoint = LIR::LastNode(node->GetOperandArray(halfArgCnt), halfArgCnt);

        GenTree* lo = comp->gtNewSimdHWIntrinsicNode(halfType, node->GetOperandArray(), halfArgCnt, halfCreate,
                                                     simdBaseJitType, simdSize / 2);

        GenTree* hi = comp->gtNewSimdHWIntrinsicNode(halfType, node->GetOperandArray(halfArgCnt), halfArgCnt,
                                                     halfCreate, simdBaseJitType, simdSize / 2);

        node->ResetHWIntrinsicId(withUpper, comp, lo, hi);

        BlockRange().InsertAfter(loInsertionPoint, lo);
        BlockRange().InsertAfter(hiInsertionPoint, hi);

        LowerNode(lo);
        LowerNode(hi);

        return LowerNode(node);
    }

    assert(intrinsicId == NI_Vector128_Create);

    // We will be constructing the following parts:
    //          /--*  op1  T
    //   tmp1 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
    //   ...

    // This is roughly the following managed code:
    //   var tmp1 = Vector128.CreateScalarUnsafe(op1);
    //   ...

    tmp1 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, op1, simdBaseJitType, 16);
    LowerNode(tmp1);

    switch (simdBaseType)
    {
        case TYP_BYTE:
        case TYP_UBYTE:
        case TYP_SHORT:
        case TYP_USHORT:
        case TYP_INT:
        case TYP_UINT:
        {
            NamedIntrinsic insIntrinsic = NI_Illegal;

            if ((simdBaseType == TYP_SHORT) || (simdBaseType == TYP_USHORT))
            {
                insIntrinsic = NI_X86Base_Insert;
            }
            else if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
            {
                insIntrinsic = NI_SSE42_Insert;
            }

            if (insIntrinsic != NI_Illegal)
            {
                for (size_t N = 1; N < argCnt - 1; N++)
                {
                    // We will be constructing the following parts:
                    //   ...
                    //   idx  =    CNS_INT       int    N
                    //          /--*  tmp1 simd16
                    //          +--*  opN  T
                    //          +--*  idx  int
                    //   tmp1 = *  HWINTRINSIC   simd16 T Insert
                    //   ...

                    // This is roughly the following managed code:
                    //   ...
                    //   tmp1 = Sse?.Insert(tmp1, opN, N);
                    //   ...

                    GenTree* opN = node->Op(N + 1);

                    idx = comp->gtNewIconNode(N, TYP_INT);
                    // Place the insert as early as possible to avoid creating a lot of long lifetimes.
                    GenTree* insertionPoint = LIR::LastNode(tmp1, opN);

                    tmp1 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp1, opN, idx, insIntrinsic, simdBaseJitType,
                                                          simdSize);
                    BlockRange().InsertAfter(insertionPoint, idx, tmp1);
                    LowerNode(tmp1);
                }

                // We will be constructing the following parts:
                //   idx  =    CNS_INT       int    (argCnt - 1)
                //          /--*  tmp1   simd16
                //          +--*  lastOp T
                //          +--*  idx    int
                //   node = *  HWINTRINSIC   simd16 T Insert

                // This is roughly the following managed code:
                //   ...
                //   tmp1 = Sse?.Insert(tmp1, lastOp, (argCnt - 1));
                //   ...

                GenTree* lastOp = node->Op(argCnt);

                idx = comp->gtNewIconNode((argCnt - 1), TYP_INT);
                BlockRange().InsertAfter(lastOp, idx);

                node->ResetHWIntrinsicId(insIntrinsic, comp, tmp1, lastOp, idx);
                break;
            }

            assert((simdBaseType != TYP_SHORT) && (simdBaseType != TYP_USHORT));

            GenTree* op[16];
            op[0] = tmp1;

            for (size_t N = 1; N < argCnt; N++)
            {
                GenTree* opN = node->Op(N + 1);

                op[N] = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, opN, simdBaseJitType, 16);
                LowerNode(op[N]);
            }

            if ((simdBaseType == TYP_BYTE) || (simdBaseType == TYP_UBYTE))
            {
                for (size_t N = 0; N < argCnt; N += 4)
                {
                    // We will be constructing the following parts:
                    //   ...
                    //          /--*  opN  T
                    //   opN  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
                    //          /--*  opO  T
                    //   opO  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
                    //          /--*  opN  simd16
                    //          +--*  opO  simd16
                    //   tmp1 = *  HWINTRINSIC   simd16 T UnpackLow
                    //          /--*  opP  T
                    //   opP  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
                    //          /--*  opQ  T
                    //   opQ  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
                    //          /--*  opP  simd16
                    //          +--*  opQ  simd16
                    //   tmp2 = *  HWINTRINSIC   simd16 T UnpackLow
                    //          /--*  tmp1 simd16
                    //          +--*  tmp2 simd16
                    //   tmp3  = *  HWINTRINSIC   simd16 T UnpackLow
                    //   ...

                    // This is roughly the following managed code:
                    //   ...
                    //   tmp1 = Sse2.UnpackLow(opN, opO);
                    //   tmp2 = Sse2.UnpackLow(opP, opQ);
                    //   tmp3 = Sse2.UnpackLow(tmp1, tmp2);
                    //   ...

                    size_t O = N + 1;
                    size_t P = N + 2;
                    size_t Q = N + 3;

                    tmp1 = comp->gtNewSimdHWIntrinsicNode(simdType, op[N], op[O], NI_X86Base_UnpackLow,
                                                          CORINFO_TYPE_UBYTE, simdSize);
                    BlockRange().InsertAfter(LIR::LastNode(op[N], op[O]), tmp1);
                    LowerNode(tmp1);

                    tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, op[P], op[Q], NI_X86Base_UnpackLow,
                                                          CORINFO_TYPE_UBYTE, simdSize);
                    BlockRange().InsertAfter(LIR::LastNode(op[P], op[Q]), tmp2);
                    LowerNode(tmp2);

                    tmp3 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp1, tmp2, NI_X86Base_UnpackLow,
                                                          CORINFO_TYPE_USHORT, simdSize);
                    BlockRange().InsertAfter(LIR::LastNode(tmp1, tmp2), tmp3);
                    LowerNode(tmp3);

                    // This caches the result in index 0 through 3, depending on which
                    // loop iteration this is and allows the rest of the logic to be
                    // shared with the TYP_INT and TYP_UINT path.

                    op[N / 4] = tmp3;
                }
            }

            // We will be constructing the following parts:
            //   ...
            //          /--*  opN  T
            //   opN  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opO  T
            //   opO  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opN  simd16
            //          +--*  opO  simd16
            //   tmp1 = *  HWINTRINSIC   simd16 T UnpackLow
            //          /--*  opP  T
            //   opP  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opQ  T
            //   opQ  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opP  simd16
            //          +--*  opQ  simd16
            //   tmp2 = *  HWINTRINSIC   simd16 T UnpackLow
            //          /--*  tmp1 simd16
            //          +--*  tmp2 simd16
            //   node = *  HWINTRINSIC   simd16 T UnpackLow

            // This is roughly the following managed code:
            //   ...
            //   tmp1 = Sse2.UnpackLow(opN, opO);
            //   tmp2 = Sse2.UnpackLow(opP, opQ);
            //   return Sse2.UnpackLow(tmp1, tmp2);

            tmp1 = comp->gtNewSimdHWIntrinsicNode(simdType, op[0], op[1], NI_X86Base_UnpackLow, CORINFO_TYPE_UINT,
                                                  simdSize);
            BlockRange().InsertAfter(LIR::LastNode(op[0], op[1]), tmp1);
            LowerNode(tmp1);

            tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, op[2], op[3], NI_X86Base_UnpackLow, CORINFO_TYPE_UINT,
                                                  simdSize);
            BlockRange().InsertAfter(LIR::LastNode(op[2], op[3]), tmp2);
            LowerNode(tmp2);

            node->ResetHWIntrinsicId(NI_X86Base_UnpackLow, tmp1, tmp2);
            node->SetSimdBaseJitType(CORINFO_TYPE_ULONG);
            break;
        }

        case TYP_FLOAT:
        {
            unsigned N   = 0;
            GenTree* opN = nullptr;

            if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
            {
                assert(argCnt <= 4);
                GenTree* insertedNodes[4];

                for (N = 1; N < argCnt - 1; N++)
                {
                    // We will be constructing the following parts:
                    //   ...
                    //
                    //          /--*  opN  T
                    //   tmp2 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
                    //   idx  =    CNS_INT       int    N
                    //          /--*  tmp1 simd16
                    //          +--*  opN  T
                    //          +--*  idx  int
                    //   tmp1 = *  HWINTRINSIC   simd16 T Insert
                    //   ...

                    // This is roughly the following managed code:
                    //   ...
                    //   tmp2 = Vector128.CreateScalarUnsafe(opN);
                    //   tmp1 = Sse41.Insert(tmp1, tmp2, N << 4);
                    //   ...

                    opN = node->Op(N + 1);

                    tmp2 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, opN, simdBaseJitType, 16);
                    LowerNode(tmp2);

                    idx = comp->gtNewIconNode(N << 4, TYP_INT);

                    // Place the insert as early as possible to avoid creating a lot of long lifetimes.
                    GenTree* insertionPoint = LIR::LastNode(tmp1, tmp2);

                    tmp3 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp1, tmp2, idx, NI_SSE42_Insert, simdBaseJitType,
                                                          simdSize);
                    BlockRange().InsertAfter(insertionPoint, idx, tmp3);

                    insertedNodes[N] = tmp3;
                    tmp1             = tmp3;
                }

                // We will be constructing the following parts:
                //   ...
                //
                //          /--*  opN  T
                //   tmp2 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
                //   idx  =    CNS_INT       int    N
                //          /--*  tmp1 simd16
                //          +--*  opN  T
                //          +--*  idx  int
                //   node = *  HWINTRINSIC   simd16 T Insert

                // This is roughly the following managed code:
                //   ...
                //   tmp2 = Vector128.CreateScalarUnsafe(opN);
                //   return Sse41.Insert(tmp1, tmp2, N << 4);

                opN = node->Op(argCnt);

                tmp2 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, opN, simdBaseJitType, 16);
                LowerNode(tmp2);

                idx = comp->gtNewIconNode((argCnt - 1) << 4, TYP_INT);
                BlockRange().InsertAfter(tmp2, idx);

                node->ResetHWIntrinsicId(NI_SSE42_Insert, comp, tmp1, tmp2, idx);

                for (N = 1; N < argCnt - 1; N++)
                {
                    // LowerNode for NI_SSE42_Insert specially handles zeros, constants, and certain mask values
                    // to do the minimal number of operations and may merge together two neighboring inserts that
                    // don't have any side effects between them. Because of this and because of the interdependence
                    // of the inserts we've created above, we need to wait to lower the generated inserts until after
                    // we've completed the chain.

                    GenTree* insertedNode = insertedNodes[N];
                    LowerNode(insertedNode);
                }
                break;
            }

            // We will be constructing the following parts:
            //   ...
            //          /--*  opN  T
            //   opN  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opO  T
            //   opO  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opN  simd16
            //          +--*  opO  simd16
            //   tmp1 = *  HWINTRINSIC   simd16 T UnpackLow
            //          /--*  opP  T
            //   opP  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opQ  T
            //   opQ  = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  opP  simd16
            //          +--*  opQ  simd16
            //   tmp2 = *  HWINTRINSIC   simd16 T UnpackLow
            //          /--*  tmp1 simd16
            //          +--*  tmp2 simd16
            //   node = *  HWINTRINSIC   simd16 T MoveLowToHigh

            // This is roughly the following managed code:
            //   ...
            //   tmp1 = Sse.UnpackLow(opN, opO);
            //   tmp2 = Sse.UnpackLow(opP, opQ);
            //   return Sse.MoveLowToHigh(tmp1, tmp2);

            GenTree* op[4];
            op[0] = tmp1;

            for (N = 1; N < argCnt; N++)
            {
                opN = node->Op(N + 1);

                op[N] = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, opN, simdBaseJitType, 16);
                LowerNode(op[N]);
            }

            tmp1 =
                comp->gtNewSimdHWIntrinsicNode(simdType, op[0], op[1], NI_X86Base_UnpackLow, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(LIR::LastNode(op[0], op[1]), tmp1);
            LowerNode(tmp1);

            tmp2 =
                comp->gtNewSimdHWIntrinsicNode(simdType, op[2], op[3], NI_X86Base_UnpackLow, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(LIR::LastNode(op[2], op[3]), tmp2);
            LowerNode(tmp2);

            node->ResetHWIntrinsicId(NI_X86Base_MoveLowToHigh, tmp1, tmp2);
            break;
        }

        case TYP_LONG:
        case TYP_ULONG:
        case TYP_DOUBLE:
        {
            GenTree* op2 = node->Op(2);

            if (varTypeIsLong(simdBaseType) && comp->compOpportunisticallyDependsOn(InstructionSet_SSE42_X64))
            {
                // We will be constructing the following parts:
                //   ...
                //   idx  =    CNS_INT       int    1
                //          /--*  tmp1 simd16
                //          +--*  op2  T
                //          +--*  idx  int
                //   node = *  HWINTRINSIC   simd16 T Insert

                // This is roughly the following managed code:
                //   ...
                //   return Sse41.X64.Insert(tmp1, op2, 0x01);

                idx = comp->gtNewIconNode(0x01, TYP_INT);
                BlockRange().InsertBefore(node, idx);

                node->ResetHWIntrinsicId(NI_SSE42_X64_Insert, comp, tmp1, op2, idx);
                break;
            }

            // We will be constructing the following parts:
            //   ...
            //          /--*  op2  T
            //   tmp2 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe
            //          /--*  tmp1 simd16
            //          +--*  tmp2 simd16
            //   node = *  HWINTRINSIC   simd16 T UnpackLow

            // This is roughly the following managed code:
            //   ...
            //   var tmp2 = Vector128.CreateScalarUnsafe(op2);
            //   return Sse.UnpackLow(tmp1, tmp2);

            tmp2 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, op2, simdBaseJitType, 16);
            LowerNode(tmp2);

            node->ResetHWIntrinsicId(NI_X86Base_UnpackLow, tmp1, tmp2);
            break;
        }

        default:
        {
            unreached();
        }
    }

    return LowerNode(node);
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicGetElement: Lowers a vector GetElement call
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsicGetElement(GenTreeHWIntrinsic* node)
{
    NamedIntrinsic intrinsicId     = node->GetHWIntrinsicId();
    var_types      simdType        = node->gtType;
    CorInfoType    simdBaseJitType = node->GetSimdBaseJitType();
    var_types      simdBaseType    = node->GetSimdBaseType();
    unsigned       simdSize        = node->GetSimdSize();

    assert(HWIntrinsicInfo::IsVectorGetElement(intrinsicId));
    assert(!varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);

    GenTree* op1 = node->Op(1);
    GenTree* op2 = node->Op(2);

    if (op2->IsIntegralConst(0))
    {
        // Specially handle as ToScalar
        BlockRange().Remove(op2);

        if (simdSize == 64)
        {
            intrinsicId = NI_Vector512_ToScalar;
        }
        else if (simdSize == 32)
        {
            intrinsicId = NI_Vector256_ToScalar;
        }
        else
        {
            intrinsicId = NI_Vector128_ToScalar;
        }

        node->ResetHWIntrinsicId(intrinsicId, op1);
        return LowerNode(node);
    }

    uint32_t elemSize = genTypeSize(simdBaseType);
    uint32_t count    = simdSize / elemSize;

    if (op1->OperIs(GT_IND))
    {
        // We want to optimize GetElement down to an Indir where possible as
        // this unlocks additional containment opportunities for various nodes

        GenTree* newBase;
        GenTree* newIndex;
        uint32_t newScale;
        int32_t  newOffset;

        // Normally we'd evaluate op1 (indir), then op2 (element index).
        // We like to be able to reorder these to fold op2 into the indir.

        GenTreeIndir* indir                = op1->AsIndir();
        GenTree*      addr                 = indir->Addr();
        bool const    canMoveTheIndirLater = IsInvariantInRange(indir, node);

        // If we can't move the indir, force evaluation of its side effects.
        //
        if (!canMoveTheIndirLater)
        {
            // Force evaluation of the address, if it is complex
            //
            if (!(addr->IsInvariant() || addr->OperIsLocal()))
            {
                addr->ClearContained();
                LIR::Use addrUse(BlockRange(), &indir->Addr(), indir);
                addrUse.ReplaceWithLclVar(comp);
                addr = indir->Addr();
            }

            // If the indir can fault, do a null check.
            //
            if (indir->OperMayThrow(comp))
            {
                GenTree* addrClone = comp->gtCloneExpr(addr);
                GenTree* nullcheck = comp->gtNewNullCheck(addrClone, comp->compCurBB);
                BlockRange().InsertBefore(indir, addrClone, nullcheck);
                LowerNode(nullcheck);

                indir->gtFlags |= GTF_IND_NONFAULTING;
            }

            // We should now be able to move the indir
            //
            indir->gtFlags &= ~GTF_EXCEPT;
        }

        if (addr->OperIsAddrMode())
        {
            // We have an existing addressing mode, so we want to try and
            // combine with that where possible to keep things as a 1x LEA

            GenTreeAddrMode* addrMode = addr->AsAddrMode();

            newBase   = addrMode->Base();
            newIndex  = addrMode->Index();
            newScale  = addrMode->GetScale();
            newOffset = addrMode->Offset();

            if (op2->OperIsConst() && (newOffset < (INT32_MAX - static_cast<int>(simdSize))))
            {
                // op2 is a constant, so add it to the existing offset

                BlockRange().Remove(addrMode);
                BlockRange().Remove(op2);

                int32_t addOffset = (static_cast<uint8_t>(op2->AsIntCon()->IconValue()) % count);
                addOffset *= static_cast<int32_t>(elemSize);

                newOffset += addOffset;
            }
            else if (newIndex == nullptr)
            {
                // op2 is not a constant and the addressing mode doesn't
                // have its own existing index, so use our index and scale

                BlockRange().Remove(addrMode);

                newIndex = op2;
                newScale = elemSize;
            }
            else if (addrMode->GetScale() == elemSize)
            {
                // op2 is not a constant but the addressing mode has its
                // own already with a matching scale, so add ours to theirs

                BlockRange().Remove(addrMode);

                newIndex = comp->gtNewOperNode(GT_ADD, TYP_I_IMPL, newIndex, op2);
                BlockRange().InsertBefore(node, newIndex);

                LowerNode(newIndex);
            }
            else
            {
                // op2 is not a constant but the addressing mode is already
                // complex, so build a new addressing mode with the prev as our base

                newBase   = addrMode;
                newIndex  = op2;
                newScale  = elemSize;
                newOffset = 0;
            }
        }
        else if (op2->OperIsConst())
        {
            // We don't have an addressing mode, so build one with the old addr
            // as the base and the offset using the op2 constant and scale

            BlockRange().Remove(op2);

            newBase   = addr;
            newIndex  = nullptr;
            newScale  = 0;
            newOffset = (static_cast<uint8_t>(op2->AsIntCon()->IconValue()) % count);
            newOffset *= static_cast<int32_t>(elemSize);
        }
        else
        {
            // We don't have an addressing mode, so build one with the old addr
            // as the base and the index set to op2

            newBase   = addr;
            newIndex  = op2;
            newScale  = elemSize;
            newOffset = 0;
        }

        if (newBase != nullptr)
        {
            newBase->ClearContained();
        }

        if (newIndex != nullptr)
        {
            newIndex->ClearContained();
        }

        GenTreeAddrMode* newAddr =
            new (comp, GT_LEA) GenTreeAddrMode(addr->TypeGet(), newBase, newIndex, newScale, newOffset);
        BlockRange().InsertBefore(node, newAddr);

        GenTreeIndir* newIndir =
            comp->gtNewIndir(JITtype2varType(simdBaseJitType), newAddr, (indir->gtFlags & GTF_IND_FLAGS));
        BlockRange().InsertBefore(node, newIndir);

        LIR::Use use;
        if (BlockRange().TryGetUse(node, &use))
        {
            use.ReplaceWith(newIndir);
        }
        else
        {
            newIndir->SetUnusedValue();
        }

        BlockRange().Remove(op1);
        BlockRange().Remove(node);

        assert(newAddr->gtNext == newIndir);
        return LowerNode(newAddr);
    }

    if (!op2->OperIsConst())
    {
        // We will specially handle GetElement in codegen when op2 isn't a constant
        ContainCheckHWIntrinsic(node);
        return node->gtNext;
    }

    // We should have a bounds check inserted for any index outside the allowed range
    // but we need to generate some code anyways, and so we'll simply mask here for simplicity.

    uint32_t imm8      = static_cast<uint8_t>(op2->AsIntCon()->IconValue()) % count;
    uint32_t simd16Cnt = 16 / elemSize;
    uint32_t simd16Idx = imm8 / simd16Cnt;

    assert((0 <= imm8) && (imm8 < count));

    if (IsContainableMemoryOp(op1))
    {
        // We will specially handle GetElement when op1 is already in memory

        if (op1->OperIs(GT_LCL_VAR, GT_LCL_FLD))
        {
            // We want to optimize GetElement down to a LclFld where possible as
            // this unlocks additional containment opportunities for various nodes

            GenTreeLclVarCommon* lclVar  = op1->AsLclVarCommon();
            uint32_t             lclOffs = lclVar->GetLclOffs() + (imm8 * elemSize);
            LclVarDsc*           lclDsc  = comp->lvaGetDesc(lclVar);

            if (lclDsc->lvDoNotEnregister && (lclOffs <= 0xFFFF) && ((lclOffs + elemSize) <= lclDsc->lvExactSize()))
            {
                GenTree* lclFld = comp->gtNewLclFldNode(lclVar->GetLclNum(), JITtype2varType(simdBaseJitType),
                                                        static_cast<uint16_t>(lclOffs));
                BlockRange().InsertBefore(node, lclFld);

                LIR::Use use;
                if (BlockRange().TryGetUse(node, &use))
                {
                    use.ReplaceWith(lclFld);
                }
                else
                {
                    lclFld->SetUnusedValue();
                }

                BlockRange().Remove(op1);
                BlockRange().Remove(op2);
                BlockRange().Remove(node);

                return LowerNode(lclFld);
            }
        }

        if (IsSafeToContainMem(node, op1))
        {
            // Handle other cases in codegen

            op2->AsIntCon()->SetIconValue(imm8);
            ContainCheckHWIntrinsic(node);

            return node->gtNext;
        }
    }

    // Remove the index node up front to simplify downstream logic
    BlockRange().Remove(op2);

    // Spare GenTrees to be used for the lowering logic below
    // Defined upfront to avoid naming conflicts, etc...
    GenTree* idx  = nullptr;
    GenTree* tmp1 = nullptr;
    GenTree* tmp2 = nullptr;

    if (intrinsicId == NI_Vector512_GetElement)
    {
        assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX512));

        if (simd16Idx == 0)
        {
            // We will be constructing the following parts:
            //   ...
            //         /--*  op1  simd64
            //   op1 = *  HWINTRINSIC   simd64 T GetLower128

            // This is roughly the following managed code:
            //   ...
            //   op1 = op1.GetLower().GetLower();

            tmp1 = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, NI_Vector512_GetLower128, simdBaseJitType, simdSize);
            BlockRange().InsertBefore(node, tmp1);
            LowerNode(tmp1);
        }
        else
        {
            assert((simd16Idx >= 1) && (simd16Idx <= 3));

            // We will be constructing the following parts:
            //   ...

            //          /--*  op1  simd64
            //          +--*  idx  int
            //   tmp1 = *  HWINTRINSIC   simd64 T ExtractVector128

            // This is roughly the following managed code:
            //   ...
            //   tmp1  = Avx512F.ExtractVector128(op1, idx);

            imm8 -= (simd16Idx * simd16Cnt);

            idx = comp->gtNewIconNode(simd16Idx);
            BlockRange().InsertBefore(node, idx);
            LowerNode(idx);

            NamedIntrinsic extractIntrinsicId = NI_AVX512_ExtractVector128;

            tmp1 = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, idx, extractIntrinsicId, simdBaseJitType, simdSize);
            BlockRange().InsertBefore(node, tmp1);
            LowerNode(tmp1);
        }

        op1 = tmp1;
    }
    else if (intrinsicId == NI_Vector256_GetElement)
    {
        assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));

        if (simd16Idx == 0)
        {
            // We will be constructing the following parts:
            //   ...
            //         /--*  op1  simd32
            //   op1 = *  HWINTRINSIC   simd32 T GetLower

            // This is roughly the following managed code:
            //   ...
            //   op1 = op1.GetLower();

            tmp1 = comp->gtNewSimdGetLowerNode(TYP_SIMD16, op1, simdBaseJitType, simdSize);
            BlockRange().InsertBefore(node, tmp1);
            LowerNode(tmp1);
        }
        else
        {
            assert(simd16Idx == 1);

            // We will be constructing the following parts:
            //   ...

            //          /--*  op1   simd32
            //   tmp1 = *  HWINTRINSIC   simd32 T GetUpper

            // This is roughly the following managed code:
            //   ...
            //   tmp1  = op1.GetUpper();

            imm8 -= count / 2;

            tmp1 = comp->gtNewSimdGetUpperNode(TYP_SIMD16, op1, simdBaseJitType, simdSize);
            BlockRange().InsertBefore(node, tmp1);
            LowerNode(tmp1);
        }

        op1 = tmp1;
    }

    NamedIntrinsic resIntrinsic = NI_Illegal;

    if (imm8 == 0)
    {
        // Specially handle as ToScalar

        node->SetSimdSize(16);
        node->ResetHWIntrinsicId(NI_Vector128_ToScalar, op1);

        return LowerNode(node);
    }
    else
    {
        op2 = comp->gtNewIconNode(imm8);
        BlockRange().InsertBefore(node, op2);

        switch (simdBaseType)
        {
            case TYP_LONG:
            case TYP_ULONG:
            {
                resIntrinsic = NI_SSE42_X64_Extract;
                break;
            }

            case TYP_FLOAT:
            case TYP_DOUBLE:
            {
                // We specially handle float and double for more efficient codegen
                resIntrinsic = NI_Vector128_GetElement;
                break;
            }

            case TYP_BYTE:
            case TYP_UBYTE:
            case TYP_INT:
            case TYP_UINT:
            {
                resIntrinsic = NI_SSE42_Extract;
                break;
            }

            case TYP_SHORT:
            case TYP_USHORT:
            {
                resIntrinsic = NI_X86Base_Extract;
                break;
            }

            default:
                unreached();
        }

        node->SetSimdSize(16);
        node->ResetHWIntrinsicId(resIntrinsic, op1, op2);
    }

    GenTree* next = node->gtNext;

    if (node->GetHWIntrinsicId() != intrinsicId)
    {
        next = LowerNode(node);
    }
    else
    {
        ContainCheckHWIntrinsic(node);
    }

    if ((simdBaseType == TYP_BYTE) || (simdBaseType == TYP_SHORT))
    {
        // The extract intrinsics zero the upper bits, so we need an explicit
        // cast to ensure the result is properly sign extended

        LIR::Use use;

        bool foundUse     = BlockRange().TryGetUse(node, &use);
        bool fromUnsigned = false;

        GenTreeCast* cast = comp->gtNewCastNode(TYP_INT, node, fromUnsigned, simdBaseType);
        BlockRange().InsertAfter(node, cast);

        if (foundUse)
        {
            use.ReplaceWith(cast);
        }
        else
        {
            node->ClearUnusedValue();
            cast->SetUnusedValue();
        }
        next = LowerNode(cast);
    }

    return next;
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicWithElement: Lowers a Vector128 or Vector256 WithElement call
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsicWithElement(GenTreeHWIntrinsic* node)
{
    NamedIntrinsic intrinsicId     = node->GetHWIntrinsicId();
    var_types      simdType        = node->TypeGet();
    CorInfoType    simdBaseJitType = node->GetSimdBaseJitType();
    var_types      simdBaseType    = node->GetSimdBaseType();
    unsigned       simdSize        = node->GetSimdSize();

    assert(varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);

    GenTree* op1 = node->Op(1);
    GenTree* op2 = node->Op(2);
    GenTree* op3 = node->Op(3);

    if (!op2->OperIsConst())
    {
        // We will specially handle WithElement in codegen when op2 isn't a constant
        ContainCheckHWIntrinsic(node);
        return node->gtNext;
    }

    // We should have a bounds check inserted for any index outside the allowed range
    // but we need to generate some code anyways, and so we'll simply mask here for simplicity.

    uint32_t elemSize = genTypeSize(simdBaseType);
    uint32_t count    = simdSize / elemSize;

    uint32_t imm8      = static_cast<uint8_t>(op2->AsIntCon()->IconValue()) % count;
    uint32_t simd16Cnt = 16 / elemSize;
    uint32_t simd16Idx = imm8 / simd16Cnt;

    assert((0 <= imm8) && (imm8 < count));

    // Remove the index node up front to simplify downstream logic
    BlockRange().Remove(op2);

    // Spare GenTrees to be used for the lowering logic below
    // Defined upfront to avoid naming conflicts, etc...
    GenTree*            idx    = nullptr;
    GenTree*            tmp1   = nullptr;
    GenTree*            tmp2   = nullptr;
    GenTreeHWIntrinsic* result = node;

    if (intrinsicId == NI_Vector512_WithElement)
    {
        // If we have a simd64 WithElement, we will spill the original
        // simd64 source into a local, extract the relevant simd16 from
        // it and then operate on that. At the end, we will insert the simd16
        // result back into the simd64 local, producing our final value.

        assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX512));

        // This copy of "node" will have the simd16 value we need.
        result = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, op2, op3, intrinsicId, simdBaseJitType, 16);
        BlockRange().InsertBefore(node, result);

        // We will be constructing the following parts:
        //   ...
        //          /--*  op1 simd64
        //          *  STORE_LCL_VAR simd64
        //  tmp64 =    LCL_VAR       simd64
        //  op1   =    LCL_VAR       simd64

        // TODO-CQ: move the tmp64 node closer to the final InsertVector128.
        LIR::Use op1Use(BlockRange(), &node->Op(1), node);
        ReplaceWithLclVar(op1Use);
        GenTree* tmp64 = node->Op(1);

        op1 = comp->gtClone(tmp64);
        BlockRange().InsertBefore(op3, op1);

        if (simd16Idx == 0)
        {
            // We will be constructing the following parts:
            //   ...
            //         /--*  op1  simd64
            //   op1 = *  HWINTRINSIC   simd64 T GetLower128

            // This is roughly the following managed code:
            //   ...
            //   op1 = op1.GetLower().GetLower();

            tmp1 = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, NI_Vector512_GetLower128, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(op1, tmp1);
            LowerNode(tmp1);
        }
        else
        {
            assert((simd16Idx >= 1) && (simd16Idx <= 3));

            // We will be constructing the following parts:
            //   ...

            //          /--*  op1  simd64
            //          +--*  idx  int
            //   tmp1 = *  HWINTRINSIC   simd64 T ExtractVector128

            // This is roughly the following managed code:
            //   ...
            //   tmp1  = Avx512F.ExtractVector128(op1, idx);

            imm8 -= (simd16Idx * simd16Cnt);

            idx = comp->gtNewIconNode(simd16Idx);
            BlockRange().InsertAfter(op1, idx);
            LowerNode(idx);

            NamedIntrinsic extractIntrinsicId = NI_AVX512_ExtractVector128;

            tmp1 = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, idx, extractIntrinsicId, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(idx, tmp1);
            LowerNode(tmp1);
        }

        op1 = tmp1;

        // Now we will insert our "result" into our simd64 temporary.

        idx = comp->gtNewIconNode(simd16Idx);
        BlockRange().InsertBefore(node, idx);
        LowerNode(idx);

        NamedIntrinsic insertIntrinsicId = NI_AVX512_InsertVector128;

        node->ResetHWIntrinsicId(insertIntrinsicId, comp, tmp64, result, idx);
    }
    else if (intrinsicId == NI_Vector256_WithElement)
    {
        // If we have a simd32 WithElement, we will spill the original
        // simd32 source into a local, extract the lower/upper half from
        // it and then operate on that. At the end, we will insert the simd16
        // result back into the simd32 local, producing our final value.

        assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));

        // This copy of "node" will have the simd16 value we need.
        result = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, op2, op3, intrinsicId, simdBaseJitType, 16);
        BlockRange().InsertBefore(node, result);

        // We will be constructing the following parts:
        //   ...
        //          /--*  op1 simd32
        //          *  STORE_LCL_VAR simd32
        //  tmp32 =    LCL_VAR       simd32
        //  op1   =    LCL_VAR       simd32

        // TODO-CQ: move the tmp32 node closer to the final InsertVector128.
        LIR::Use op1Use(BlockRange(), &node->Op(1), node);
        ReplaceWithLclVar(op1Use);
        GenTree* tmp32 = node->Op(1);

        op1 = comp->gtClone(tmp32);
        BlockRange().InsertBefore(op3, op1);

        if (simd16Idx == 0)
        {
            // We will be constructing the following parts:
            //   ...
            //         /--*  op1  simd32
            //   op1 = *  HWINTRINSIC   simd32 T GetLower

            // This is roughly the following managed code:
            //   ...
            //   op1 = op1.GetLower();

            tmp1 = comp->gtNewSimdGetLowerNode(TYP_SIMD16, op1, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(op1, tmp1);
            LowerNode(tmp1);
        }
        else
        {
            assert(simd16Idx == 1);

            // We will be constructing the following parts:
            //   ...

            //          /--*  op1   simd32
            //   tmp1 = *  HWINTRINSIC   simd32 T GetUpper

            // This is roughly the following managed code:
            //   ...
            //   tmp1  = op1.GetUpper();

            imm8 -= count / 2;

            tmp1 = comp->gtNewSimdGetUpperNode(TYP_SIMD16, op1, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(op1, tmp1);
            LowerNode(tmp1);
        }

        op1 = tmp1;

        // Now we will insert our "result" into our simd32 temporary.
        if (simd16Idx == 0)
        {
            node->ResetHWIntrinsicId(NI_Vector256_WithLower, comp, tmp32, result);
        }
        else
        {
            node->ResetHWIntrinsicId(NI_Vector256_WithUpper, comp, tmp32, result);
        }
    }
    else
    {
        assert(simd16Idx == 0);
    }

    switch (simdBaseType)
    {
        case TYP_LONG:
        case TYP_ULONG:
        {
            assert(comp->compIsaSupportedDebugOnly(InstructionSet_SSE42_X64));

            idx = comp->gtNewIconNode(imm8);
            BlockRange().InsertBefore(result, idx);
            result->ChangeHWIntrinsicId(NI_SSE42_X64_Insert, op1, op3, idx);
            break;
        }

        case TYP_FLOAT:
        {
            // We will be constructing the following parts:
            //   ...
            //          /--*  op3   float
            //   tmp1 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe

            // This is roughly the following managed code:
            //   ...
            //   tmp1 = Vector128.CreateScalarUnsafe(op3);

            tmp1 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, op3, CORINFO_TYPE_FLOAT, 16);
            LowerNode(tmp1);

            if (!comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
            {
                if (imm8 == 0)
                {
                    // We will be constructing the following parts:
                    //   ...
                    //          /--*  op1   simd16
                    //          +--*  op2   simd16
                    //   node = *  HWINTRINSIC   simd16 T MoveScalar

                    // This is roughly the following managed code:
                    //   ...
                    //   node  = Sse.MoveScalar(op1, op2);

                    result->ResetHWIntrinsicId(NI_X86Base_MoveScalar, op1, tmp1);
                }
                else
                {
                    // We will be constructing the following parts:
                    //   ...
                    //          /--*  op1 simd16
                    //          *  STORE_LCL_VAR simd16
                    //   op2  =    LCL_VAR       simd16
                    //   tmp2 =    LCL_VAR       simd16
                    //   idx  =    CNS_INT       int    0
                    //          /--*  tmp1   simd16
                    //          +--*  tmp2   simd16
                    //          +--*  idx    int
                    //   op1  = *  HWINTRINSIC   simd16 T Shuffle
                    //   idx  =    CNS_INT       int    226
                    //          /--*  op1   simd16
                    //          +--*  tmp2   simd16
                    //          +--*  idx    int
                    //   op1  = *  HWINTRINSIC   simd16 T Shuffle

                    // This is roughly the following managed code:
                    //   ...
                    //   tmp2  = Sse.Shuffle(tmp1, op1,   0 or  48 or 32);
                    //   node  = Sse.Shuffle(tmp2, op1, 226 or 132 or 36);

                    result->Op(1) = op1;
                    LIR::Use op1Use(BlockRange(), &result->Op(1), result);
                    ReplaceWithLclVar(op1Use);
                    op2 = result->Op(1);

                    tmp2 = comp->gtClone(op2);
                    BlockRange().InsertAfter(tmp1, tmp2);

                    ssize_t controlBits1;
                    ssize_t controlBits2;

                    // The comments beside the control bits below are listed using the managed API operands
                    //
                    // In practice, for the first step the value being inserted (op3) is in tmp1
                    // while the other elements of the result (op1) are in tmp2. The result ends
                    // up containing the value being inserted and its immediate neighbor.
                    //
                    // The second step takes that result (which is in op1) plus the other elements
                    // from op2 (a clone of op1/tmp2 from the previous step) and combines them to
                    // create the final result.

                    switch (imm8)
                    {
                        case 1:
                        {
                            controlBits1 = 0;   // 00 00 00 00;  op1 = { X = op3,   Y = op3,   Z = op1.X, W = op1.X }
                            controlBits2 = 226; // 11 10 00 10; node = { X = op1.X, Y = op3,   Z = op1.Z, W = op1.W }
                            break;
                        }

                        case 2:
                        {
                            controlBits1 = 15; // 00 00 11 11;  op1 = { X = op1.W, Y = op1.W, Z = op3, W = op3 }
                            controlBits2 = 36; // 00 10 01 00; node = { X = op1.X, Y = op1.Y, Z = op3, W = op1.W }
                            break;
                        }

                        case 3:
                        {
                            controlBits1 = 10;  // 00 00 10 10;  op1 = { X = op1.Z, Y = op1.Z, Z = op3,   W = op3 }
                            controlBits2 = 132; // 10 00 01 00; node = { X = op1.X, Y = op1.Y, Z = op1.Z, W = op3 }
                            break;
                        }

                        default:
                            unreached();
                    }

                    idx = comp->gtNewIconNode(controlBits1);
                    BlockRange().InsertAfter(tmp2, idx);

                    if (imm8 != 1)
                    {
                        std::swap(tmp1, tmp2);
                    }

                    op1 = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, tmp1, tmp2, idx, NI_X86Base_Shuffle,
                                                         CORINFO_TYPE_FLOAT, 16);
                    BlockRange().InsertAfter(idx, op1);
                    LowerNode(op1);

                    idx = comp->gtNewIconNode(controlBits2);
                    BlockRange().InsertAfter(op1, idx);

                    if (imm8 != 1)
                    {
                        std::swap(op1, op2);
                    }

                    result->ChangeHWIntrinsicId(NI_X86Base_Shuffle, op1, op2, idx);
                }
                break;
            }
            else
            {
                imm8 = imm8 * 16;
                op3  = tmp1;
                FALLTHROUGH;
            }
        }

        case TYP_BYTE:
        case TYP_UBYTE:
        case TYP_INT:
        case TYP_UINT:
        {
            assert(comp->compIsaSupportedDebugOnly(InstructionSet_SSE42));

            idx = comp->gtNewIconNode(imm8);
            BlockRange().InsertBefore(result, idx);
            result->ChangeHWIntrinsicId(NI_SSE42_Insert, op1, op3, idx);
            break;
        }

        case TYP_SHORT:
        case TYP_USHORT:
        {
            idx = comp->gtNewIconNode(imm8);
            BlockRange().InsertBefore(result, idx);
            result->ChangeHWIntrinsicId(NI_X86Base_Insert, op1, op3, idx);
            break;
        }

        case TYP_DOUBLE:
        {
            // We will be constructing the following parts:
            //   ...
            //          /--*  op3   double
            //   tmp1 = *  HWINTRINSIC   simd16 T CreateScalarUnsafe

            // This is roughly the following managed code:
            //   ...
            //   tmp1 = Vector128.CreateScalarUnsafe(op3);

            tmp1 = InsertNewSimdCreateScalarUnsafeNode(TYP_SIMD16, op3, CORINFO_TYPE_DOUBLE, 16);
            LowerNode(tmp1);

            result->ResetHWIntrinsicId((imm8 == 0) ? NI_X86Base_MoveScalar : NI_X86Base_UnpackLow, op1, tmp1);
            break;
        }

        default:
            unreached();
    }

    assert(result->GetHWIntrinsicId() != intrinsicId);
    GenTree* nextNode = LowerNode(result);

    if (intrinsicId == NI_Vector512_WithElement)
    {
        // Now that we have finalized the shape of the tree, lower the insertion node as well.

        assert(node->GetHWIntrinsicId() == NI_AVX512_InsertVector128);
        assert(node != result);

        nextNode = LowerNode(node);
    }
    else if (intrinsicId == NI_Vector256_WithElement)
    {
        // Now that we have finalized the shape of the tree, lower the insertion node as well.

        assert((node->GetHWIntrinsicId() == NI_Vector256_WithLower) ||
               (node->GetHWIntrinsicId() == NI_Vector256_WithUpper));
        assert(node != result);

        nextNode = LowerNode(node);
    }
    else
    {
        assert(node == result);
    }

    return nextNode;
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicDot: Lowers a Vector128 or Vector256 Dot call
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsicDot(GenTreeHWIntrinsic* node)
{
    NamedIntrinsic intrinsicId     = node->GetHWIntrinsicId();
    CorInfoType    simdBaseJitType = node->GetSimdBaseJitType();
    var_types      simdBaseType    = node->GetSimdBaseType();
    unsigned       simdSize        = node->GetSimdSize();
    var_types      simdType        = Compiler::getSIMDTypeForSize(simdSize);
    unsigned       simd16Count     = comp->getSIMDVectorLength(16, simdBaseType);

    assert((intrinsicId == NI_Vector128_Dot) || (intrinsicId == NI_Vector256_Dot));
    assert(varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);
    assert(varTypeIsSIMD(node));

    GenTree* op1 = node->Op(1);
    GenTree* op2 = node->Op(2);

    // Spare GenTrees to be used for the lowering logic below
    // Defined upfront to avoid naming conflicts, etc...
    GenTree* idx  = nullptr;
    GenTree* tmp1 = nullptr;
    GenTree* tmp2 = nullptr;
    GenTree* tmp3 = nullptr;

    NamedIntrinsic horizontalAdd = NI_Illegal;
    NamedIntrinsic shuffle       = NI_Illegal;

    if (simdSize == 32)
    {
        switch (simdBaseType)
        {
            case TYP_SHORT:
            case TYP_USHORT:
            case TYP_INT:
            case TYP_UINT:
            {
                assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX2));
                horizontalAdd = NI_AVX2_HorizontalAdd;
                break;
            }

            case TYP_FLOAT:
            {
                assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));

                // We will be constructing the following parts:
                //   idx  =    CNS_INT       int    0xFF
                //          /--*  op1  simd16
                //          +--*  op2  simd16
                //          +--*  idx  int
                //   tmp1 = *  HWINTRINSIC   simd32 T DotProduct
                //          /--*  tmp1 simd32
                //          *  STORE_LCL_VAR simd32
                //   tmp1 =    LCL_VAR       simd32
                //   tmp2 =    LCL_VAR       simd32
                //   tmp3 =    LCL_VAR       simd32
                //          /--*  tmp2 simd32
                //          +--*  tmp3 simd32
                //          +--*  CNS_INT    int    0x01
                //   tmp2 = *  HWINTRINSIC   simd32 T Permute
                //          /--*  tmp1 simd32
                //          +--*  tmp2 simd32
                //   node = *  HWINTRINSIC   simd32 T Add

                // This is roughly the following managed code:
                //   var tmp1 = Avx.DotProduct(op1, op2, 0xFF);
                //   var tmp2 = Avx.Permute2x128(tmp1, tmp1, 0x4E);
                //   return Avx.Add(tmp1, tmp2);

                idx = comp->gtNewIconNode(0xFF, TYP_INT);
                BlockRange().InsertBefore(node, idx);

                tmp1 = comp->gtNewSimdHWIntrinsicNode(simdType, op1, op2, idx, NI_AVX_DotProduct, simdBaseJitType,
                                                      simdSize);
                BlockRange().InsertAfter(idx, tmp1);
                LowerNode(tmp1);

                node->Op(1) = tmp1;
                LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(tmp1Use);
                tmp1 = node->Op(1);

                tmp2 = comp->gtClone(tmp1);
                BlockRange().InsertAfter(tmp1, tmp2);

                tmp3 = comp->gtClone(tmp2);
                BlockRange().InsertAfter(tmp2, tmp3);

                idx = comp->gtNewIconNode(0x01, TYP_INT);
                BlockRange().InsertAfter(tmp3, idx);

                tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp2, tmp3, idx, NI_AVX_Permute2x128, simdBaseJitType,
                                                      simdSize);
                BlockRange().InsertAfter(idx, tmp2);
                LowerNode(tmp2);

                tmp1 = comp->gtNewSimdBinOpNode(GT_ADD, simdType, tmp1, tmp2, simdBaseJitType, simdSize);
                BlockRange().InsertAfter(tmp2, tmp1);

                // We're producing a vector result, so just return the result directly
                LIR::Use use;

                if (BlockRange().TryGetUse(node, &use))
                {
                    use.ReplaceWith(tmp1);
                }
                else
                {
                    tmp1->SetUnusedValue();
                }

                BlockRange().Remove(node);
                return LowerNode(tmp1);
            }

            case TYP_DOUBLE:
            {
                assert(comp->compIsaSupportedDebugOnly(InstructionSet_AVX));
                horizontalAdd = NI_AVX_HorizontalAdd;
                break;
            }

            default:
            {
                unreached();
            }
        }
    }
    else
    {
        switch (simdBaseType)
        {
            case TYP_SHORT:
            case TYP_USHORT:
            {
                horizontalAdd = NI_SSE42_HorizontalAdd;

                if (!comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                {
                    shuffle = NI_X86Base_ShuffleLow;
                }
                break;
            }

            case TYP_INT:
            case TYP_UINT:
            {
                assert(comp->compIsaSupportedDebugOnly(InstructionSet_SSE42));
                horizontalAdd = NI_SSE42_HorizontalAdd;
                break;
            }

            case TYP_FLOAT:
            {
                if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                {
                    // We will be constructing the following parts:
                    //   idx  =    CNS_INT       int    0xFF
                    //          /--*  op1  simd16
                    //          +--*  op2  simd16
                    //          +--*  idx  int
                    //   tmp3 = *  HWINTRINSIC   simd16 T DotProduct
                    //          /--*  tmp3 simd16
                    //   node = *  HWINTRINSIC   simd16 T ToScalar

                    // This is roughly the following managed code:
                    //   var tmp3 = Avx.DotProduct(op1, op2, 0xFF);
                    //   return tmp3.ToScalar();

                    if (simdSize == 8)
                    {
                        idx = comp->gtNewIconNode(0x3F, TYP_INT);
                    }
                    else if (simdSize == 12)
                    {
                        idx = comp->gtNewIconNode(0x7F, TYP_INT);
                    }
                    else
                    {
                        assert(simdSize == 16);
                        idx = comp->gtNewIconNode(0xFF, TYP_INT);
                    }
                    BlockRange().InsertBefore(node, idx);

                    if (varTypeIsSIMD(node->gtType))
                    {
                        // We're producing a vector result, so just emit DotProduct directly
                        node->ResetHWIntrinsicId(NI_SSE42_DotProduct, comp, op1, op2, idx);
                    }
                    else
                    {
                        // We're producing a scalar result, so we only need the result in element 0
                        //
                        // However, doing that would break/limit CSE and requires a partial write so
                        // it's better to just broadcast the value to the entire vector

                        tmp3 = comp->gtNewSimdHWIntrinsicNode(simdType, op1, op2, idx, NI_SSE42_DotProduct,
                                                              simdBaseJitType, simdSize);
                        BlockRange().InsertAfter(idx, tmp3);
                        LowerNode(tmp3);

                        node->ResetHWIntrinsicId(NI_Vector128_ToScalar, tmp3);
                    }

                    return LowerNode(node);
                }

                horizontalAdd = NI_SSE42_HorizontalAdd;

                if ((simdSize == 8) || !comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                {
                    // We also do this for simdSize == 8 to ensure we broadcast the result as expected
                    shuffle = NI_X86Base_Shuffle;
                }
                break;
            }

            case TYP_DOUBLE:
            {
                if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                {
                    // We will be constructing the following parts:
                    //   idx  =    CNS_INT       int    0x31
                    //          /--*  op1  simd16
                    //          +--*  op2  simd16
                    //          +--*  idx  int
                    //   tmp3 = *  HWINTRINSIC   simd16 T DotProduct
                    //          /--*  tmp3 simd16
                    //   node = *  HWINTRINSIC   simd16 T ToScalar

                    // This is roughly the following managed code:
                    //   var tmp3 = Avx.DotProduct(op1, op2, 0x31);
                    //   return tmp3.ToScalar();

                    idx = comp->gtNewIconNode(0x33, TYP_INT);
                    BlockRange().InsertBefore(node, idx);

                    if (varTypeIsSIMD(node->gtType))
                    {
                        // We're producing a vector result, so just emit DotProduct directly
                        node->ResetHWIntrinsicId(NI_SSE42_DotProduct, comp, op1, op2, idx);
                    }
                    else
                    {
                        // We're producing a scalar result, so we only need the result in element 0
                        //
                        // However, doing that would break/limit CSE and requires a partial write so
                        // it's better to just broadcast the value to the entire vector

                        tmp3 = comp->gtNewSimdHWIntrinsicNode(simdType, op1, op2, idx, NI_SSE42_DotProduct,
                                                              simdBaseJitType, simdSize);
                        BlockRange().InsertAfter(idx, tmp3);
                        LowerNode(tmp3);

                        node->ResetHWIntrinsicId(NI_Vector128_ToScalar, tmp3);
                    }

                    return LowerNode(node);
                }

                horizontalAdd = NI_SSE42_HorizontalAdd;

                // We need to ensure we broadcast the result as expected
                shuffle = NI_X86Base_Shuffle;
                break;
            }

            default:
            {
                unreached();
            }
        }

        if (simdSize == 8)
        {
            assert(simdBaseType == TYP_FLOAT);

            // If simdSize == 8 then we have only two elements, not the 4 that we got from getSIMDVectorLength,
            // which we gave a simdSize of 16. So, we set the simd16Count to 2 so that only 1 hadd will
            // be emitted rather than 2, so that the upper two elements will be ignored.

            simd16Count = 2;
        }
        else if (simdSize == 12)
        {
            assert(simdBaseType == TYP_FLOAT);

            // We need to mask off the most significant element to avoid the shuffle + add
            // from including it in the computed result. We need to do this for both op1 and
            // op2 in case one of them is `NaN` (because Zero * NaN == NaN)

            simd16_t simd16Val = {};

            simd16Val.i32[0] = -1;
            simd16Val.i32[1] = -1;
            simd16Val.i32[2] = -1;
            simd16Val.i32[3] = +0;

            simdType = TYP_SIMD16;
            simdSize = 16;

            // We will be constructing the following parts:
            //   ...
            //          +--*  CNS_INT    int    -1
            //          +--*  CNS_INT    int    -1
            //          +--*  CNS_INT    int    -1
            //          +--*  CNS_INT    int    0
            //   tmp1 = *  HWINTRINSIC   simd16 T Create
            //          /--*  op1 simd16
            //          +--*  tmp1 simd16
            //   op1  = *  HWINTRINSIC   simd16 T And
            //   ...

            // This is roughly the following managed code:
            //   ...
            //   tmp1 = Vector128.Create(-1, -1, -1, 0);
            //   op1  = Sse.And(op1, tmp1);
            //   ...

            GenTreeVecCon* vecCon1 = comp->gtNewVconNode(simdType);
            memcpy(&vecCon1->gtSimdVal, &simd16Val, sizeof(simd16_t));
            BlockRange().InsertAfter(op1, vecCon1);

            op1 = comp->gtNewSimdBinOpNode(GT_AND, simdType, op1, vecCon1, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(vecCon1, op1);

            LowerNode(vecCon1);
            LowerNode(op1);

            // We will be constructing the following parts:
            //   ...
            //          +--*  CNS_INT    int    -1
            //          +--*  CNS_INT    int    -1
            //          +--*  CNS_INT    int    -1
            //          +--*  CNS_INT    int    0
            //   tmp2 = *  HWINTRINSIC   simd16 T Create
            //          /--*  op2 simd16
            //          +--*  tmp2 simd16
            //   op2  = *  HWINTRINSIC   simd16 T And
            //   ...

            // This is roughly the following managed code:
            //   ...
            //   tmp2 = Vector128.Create(-1, -1, -1, 0);
            //   op2  = Sse.And(op2, tmp2);
            //   ...

            GenTreeVecCon* vecCon2 = comp->gtNewVconNode(simdType);
            memcpy(&vecCon2->gtSimdVal, &simd16Val, sizeof(simd16_t));
            BlockRange().InsertAfter(op2, vecCon2);

            op2 = comp->gtNewSimdBinOpNode(GT_AND, simdType, op2, vecCon2, simdBaseJitType, simdSize);
            BlockRange().InsertAfter(vecCon2, op2);

            LowerNode(vecCon2);
            LowerNode(op2);
        }
    }

    // We will be constructing the following parts:
    //          /--*  op1  simd16
    //          +--*  op2  simd16
    //   tmp1 = *  HWINTRINSIC   simd16 T Multiply
    //   ...

    // This is roughly the following managed code:
    //   var tmp1 = Isa.Multiply(op1, op2);
    //   ...

    tmp1 = comp->gtNewSimdBinOpNode(GT_MUL, simdType, op1, op2, simdBaseJitType, simdSize);
    BlockRange().InsertBefore(node, tmp1);
    LowerNode(tmp1);

    // HorizontalAdd combines pairs so we need log2(simd16Count) passes to sum all elements together.
    int haddCount = genLog2(simd16Count);

    for (int i = 0; i < haddCount; i++)
    {
        // We will be constructing the following parts:
        //   ...
        //          /--*  tmp1 simd16
        //          *  STORE_LCL_VAR simd16
        //   tmp1 =    LCL_VAR       simd16
        //   tmp2 =    LCL_VAR       simd16
        //   ...

        // This is roughly the following managed code:
        //   ...
        //   tmp2 = tmp1;
        //   ...

        node->Op(1) = tmp1;
        LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
        ReplaceWithLclVar(tmp1Use);
        tmp1 = node->Op(1);

        tmp2 = comp->gtClone(tmp1);
        BlockRange().InsertAfter(tmp1, tmp2);

        if (shuffle == NI_Illegal)
        {
            // We will be constructing the following parts:
            //   ...
            //          /--*  tmp1 simd16
            //          +--*  tmp2 simd16
            //   tmp1 = *  HWINTRINSIC   simd16 T HorizontalAdd
            //   ...

            // This is roughly the following managed code:
            //   ...
            //   tmp1 = Isa.HorizontalAdd(tmp1, tmp2);
            //   ...

            tmp1 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp1, tmp2, horizontalAdd, simdBaseJitType, simdSize);
        }
        else
        {
            int shuffleConst = 0x00;

            switch (i)
            {
                case 0:
                {
                    assert((simdBaseType == TYP_SHORT) || (simdBaseType == TYP_USHORT) ||
                           varTypeIsFloating(simdBaseType));

                    // Adds (e0 + e1, e1 + e0, e2 + e3, e3 + e2), giving:
                    //   e0, e1, e2, e3 | e4, e5, e6, e7
                    //   e1, e0, e3, e2 | e5, e4, e7, e6
                    //   ...

                    shuffleConst = 0xB1;
                    break;
                }

                case 1:
                {
                    assert((simdBaseType == TYP_SHORT) || (simdBaseType == TYP_USHORT) || (simdBaseType == TYP_FLOAT));

                    // Adds (e0 + e2, e1 + e3, e2 + e0, e3 + e1), giving:
                    //   ...
                    //   e2, e3, e0, e1 | e6, e7, e4, e5
                    //   e3, e2, e1, e0 | e7, e6, e5, e4

                    shuffleConst = 0x4E;
                    break;
                }

                case 2:
                {
                    assert((simdBaseType == TYP_SHORT) || (simdBaseType == TYP_USHORT));

                    // Adds (e0 + e4, e1 + e5, e2 + e6, e3 + e7), giving:
                    //   ...
                    //   e4, e5, e6, e7 | e0, e1, e2, e3
                    //   e5, e4, e7, e6 | e1, e0, e3, e2
                    //   e6, e7, e4, e5 | e2, e3, e0, e1
                    //   e7, e6, e5, e4 | e3, e2, e1, e0

                    shuffleConst = 0x4E;
                    break;
                }

                default:
                {
                    unreached();
                }
            }

            idx = comp->gtNewIconNode(shuffleConst, TYP_INT);
            BlockRange().InsertAfter(tmp2, idx);

            if (varTypeIsFloating(simdBaseType))
            {
                // We will be constructing the following parts:
                //   ...
                //          /--*  tmp2 simd16
                //          *  STORE_LCL_VAR simd16
                //   tmp2 =    LCL_VAR       simd16
                //   tmp3 =    LCL_VAR       simd16
                //   idx  =    CNS_INT       int    shuffleConst
                //          /--*  tmp2 simd16
                //          +--*  tmp3 simd16
                //          +--*  idx  simd16
                //   tmp2 = *  HWINTRINSIC   simd16 T Shuffle
                //   ...

                // This is roughly the following managed code:
                //   ...
                //   tmp3 = tmp2;
                //   tmp2 = Isa.Shuffle(tmp2, tmp3, shuffleConst);
                //   ...

                node->Op(1) = tmp2;
                LIR::Use tmp2Use(BlockRange(), &node->Op(1), node);
                ReplaceWithLclVar(tmp2Use);
                tmp2 = node->Op(1);

                tmp3 = comp->gtClone(tmp2);
                BlockRange().InsertAfter(tmp2, tmp3);

                tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp2, tmp3, idx, shuffle, simdBaseJitType, simdSize);
            }
            else
            {
                assert((simdBaseType == TYP_SHORT) || (simdBaseType == TYP_USHORT));

                if (i < 2)
                {
                    // We will be constructing the following parts:
                    //   ...
                    //   idx  =    CNS_INT       int    shuffleConst
                    //          /--*  tmp2 simd16
                    //          +--*  idx  simd16
                    //   tmp2 = *  HWINTRINSIC   simd16 T ShuffleLow
                    //   idx  =    CNS_INT       int    shuffleConst
                    //          /--*  tmp2 simd16
                    //          +--*  idx  simd16
                    //   tmp2 = *  HWINTRINSIC   simd16 T ShuffleHigh
                    //   ...

                    // This is roughly the following managed code:
                    //   ...
                    //   tmp2 = Isa.Shuffle(tmp1, shuffleConst);
                    //   ...

                    tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp2, idx, NI_X86Base_ShuffleLow, simdBaseJitType,
                                                          simdSize);
                    BlockRange().InsertAfter(idx, tmp2);
                    LowerNode(tmp2);

                    idx = comp->gtNewIconNode(shuffleConst, TYP_INT);
                    BlockRange().InsertAfter(tmp2, idx);

                    tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp2, idx, NI_X86Base_ShuffleHigh, simdBaseJitType,
                                                          simdSize);
                }
                else
                {
                    assert(i == 2);

                    // We will be constructing the following parts:
                    //   ...
                    //   idx  =    CNS_INT       int    shuffleConst
                    //          /--*  tmp2 simd16
                    //          +--*  idx  simd16
                    //   tmp2 = *  HWINTRINSIC   simd16 T ShuffleLow
                    //   ...

                    // This is roughly the following managed code:
                    //   ...
                    //   tmp2 = Isa.Shuffle(tmp1, shuffleConst);
                    //   ...

                    tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp2, idx, NI_X86Base_Shuffle, CORINFO_TYPE_INT,
                                                          simdSize);
                }
            }

            BlockRange().InsertAfter(idx, tmp2);
            LowerNode(tmp2);

            // We will be constructing the following parts:
            //   ...
            //          /--*  tmp1 simd16
            //          +--*  tmp2 simd16
            //   tmp1 = *  HWINTRINSIC   simd16 T Add
            //   ...

            // This is roughly the following managed code:
            //   ...
            //   tmp1 = Isa.Add(tmp1, tmp2);
            //   ...

            tmp1 = comp->gtNewSimdBinOpNode(GT_ADD, simdType, tmp1, tmp2, simdBaseJitType, simdSize);
        }

        BlockRange().InsertAfter(tmp2, tmp1);
        LowerNode(tmp1);
    }

    if (simdSize == 32)
    {
        // We will be constructing the following parts:
        //   ...
        //          /--*  tmp1 simd32
        //          *  STORE_LCL_VAR simd32
        //   tmp1 =    LCL_VAR       simd32
        //   tmp2 =    LCL_VAR       simd32
        //          /--*  tmp2 simd32
        //          +--*  CNS_INT    int    0x01
        //   tmp2 = *  HWINTRINSIC   simd32 float Permute
        //          /--*  tmp1 simd32
        //          +--*  tmp2 simd32
        //   tmp1 = *  HWINTRINSIC   simd32 T Add
        //   ...

        // This is roughly the following managed code:
        //   ...
        //   var tmp2 = Isa.Permute2x128(tmp1, tmp2, 0x01);
        //   tmp1 = Isa.Add(tmp1, tmp2);
        //   ...

        assert(simdBaseType != TYP_FLOAT);

        node->Op(1) = tmp1;
        LIR::Use tmp1Use(BlockRange(), &node->Op(1), node);
        ReplaceWithLclVar(tmp1Use);
        tmp1 = node->Op(1);

        tmp2 = comp->gtClone(tmp1);
        BlockRange().InsertAfter(tmp1, tmp2);

        tmp3 = comp->gtClone(tmp2);
        BlockRange().InsertAfter(tmp2, tmp3);

        idx = comp->gtNewIconNode(0x01, TYP_INT);
        BlockRange().InsertAfter(tmp3, idx);

        NamedIntrinsic permute2x128 = (simdBaseType == TYP_DOUBLE) ? NI_AVX_Permute2x128 : NI_AVX2_Permute2x128;

        tmp2 = comp->gtNewSimdHWIntrinsicNode(simdType, tmp2, tmp3, idx, permute2x128, simdBaseJitType, simdSize);
        BlockRange().InsertAfter(idx, tmp2);
        LowerNode(tmp2);

        tmp1 = comp->gtNewSimdBinOpNode(GT_ADD, simdType, tmp1, tmp2, simdBaseJitType, simdSize);
        BlockRange().InsertAfter(tmp2, tmp1);
        LowerNode(tmp1);
    }

    // We're producing a vector result, so just return the result directly
    LIR::Use use;

    if (BlockRange().TryGetUse(node, &use))
    {
        use.ReplaceWith(tmp1);
    }
    else
    {
        tmp1->SetUnusedValue();
    }

    BlockRange().Remove(node);
    return tmp1->gtNext;
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerHWIntrinsicToScalar: Lowers a Vector128 or Vector256 ToScalar call
//
//  Arguments:
//     node - The hardware intrinsic node.
//
GenTree* Lowering::LowerHWIntrinsicToScalar(GenTreeHWIntrinsic* node)
{
    NamedIntrinsic intrinsicId     = node->GetHWIntrinsicId();
    CorInfoType    simdBaseJitType = node->GetSimdBaseJitType();
    var_types      simdBaseType    = node->GetSimdBaseType();
    unsigned       simdSize        = node->GetSimdSize();
    var_types      simdType        = Compiler::getSIMDTypeForSize(simdSize);

    assert(HWIntrinsicInfo::IsVectorToScalar(intrinsicId));
    assert(varTypeIsSIMD(simdType));
    assert(varTypeIsArithmetic(simdBaseType));
    assert(simdSize != 0);

    GenTree* op1 = node->Op(1);

    if (IsContainableMemoryOp(op1) && (!varTypeIsLong(simdBaseType) || TargetArchitecture::Is64Bit))
    {
        // If op1 is already in memory, we'd like the consumer of ToScalar to be able to look
        // through to the memory directly. Early folding is preferable, as it unlocks additional
        // containment opportunities for the consuming nodes. If we can't fold away ToScalar,
        // we will still contain op1 if possible, and let codegen try to peek through to it.
        //
        // However, we specifically need to avoid doing this for long on 32-bit because we are
        // already past DecomposeLongs, and codegen wouldn't be able to handle it.

        if (op1->OperIs(GT_IND))
        {
            GenTreeIndir* indir = op1->AsIndir();

            GenTreeIndir* newIndir =
                comp->gtNewIndir(JITtype2varType(simdBaseJitType), indir->Addr(), (indir->gtFlags & GTF_IND_FLAGS));
            BlockRange().InsertBefore(node, newIndir);

            LIR::Use use;
            if (BlockRange().TryGetUse(node, &use))
            {
                use.ReplaceWith(newIndir);
            }
            else
            {
                newIndir->SetUnusedValue();
            }

            BlockRange().Remove(op1);
            BlockRange().Remove(node);

            return LowerNode(newIndir);
        }

        if (op1->OperIs(GT_LCL_VAR, GT_LCL_FLD))
        {
            uint32_t elemSize = genTypeSize(simdBaseType);

            GenTreeLclVarCommon* lclVar  = op1->AsLclVarCommon();
            uint32_t             lclOffs = lclVar->GetLclOffs() + (0 * elemSize);
            LclVarDsc*           lclDsc  = comp->lvaGetDesc(lclVar);

            if (lclDsc->lvDoNotEnregister && (lclOffs <= 0xFFFF) && ((lclOffs + elemSize) <= lclDsc->lvExactSize()))
            {
                GenTree* lclFld =
                    comp->gtNewLclFldNode(lclVar->GetLclNum(), JITtype2varType(simdBaseJitType), lclVar->GetLclOffs());
                BlockRange().InsertBefore(node, lclFld);

                LIR::Use use;
                if (BlockRange().TryGetUse(node, &use))
                {
                    use.ReplaceWith(lclFld);
                }
                else
                {
                    lclFld->SetUnusedValue();
                }

                BlockRange().Remove(op1);
                BlockRange().Remove(node);

                return LowerNode(lclFld);
            }
        }
    }

    ContainCheckHWIntrinsic(node);
    return node->gtNext;
}

//----------------------------------------------------------------------------------------------
// Lowering::TryLowerAndOpToResetLowestSetBit: Lowers a tree AND(X, ADD(X, -1)) to HWIntrinsic::ResetLowestSetBit
//
// Arguments:
//    andNode - GT_AND node of integral type
//
// Return Value:
//    Returns the replacement node if one is created else nullptr indicating no replacement
//
// Notes:
//    Performs containment checks on the replacement node if one is created
GenTree* Lowering::TryLowerAndOpToResetLowestSetBit(GenTreeOp* andNode)
{
    assert(andNode->OperIs(GT_AND) && varTypeIsIntegral(andNode));

    GenTree* op1 = andNode->gtGetOp1();
    if (!op1->OperIs(GT_LCL_VAR) || comp->lvaGetDesc(op1->AsLclVar())->IsAddressExposed())
    {
        return nullptr;
    }

    GenTree* op2 = andNode->gtGetOp2();
    if (!op2->OperIs(GT_ADD))
    {
        return nullptr;
    }

    GenTree* addOp2 = op2->gtGetOp2();
    if (!addOp2->IsIntegralConst(-1))
    {
        return nullptr;
    }

    GenTree* addOp1 = op2->gtGetOp1();
    if (!addOp1->OperIs(GT_LCL_VAR) || (addOp1->AsLclVar()->GetLclNum() != op1->AsLclVar()->GetLclNum()))
    {
        return nullptr;
    }

    // Subsequent nodes may rely on CPU flags set by these nodes in which case we cannot remove them
    if (((addOp2->gtFlags & GTF_SET_FLAGS) != 0) || ((op2->gtFlags & GTF_SET_FLAGS) != 0) ||
        ((andNode->gtFlags & GTF_SET_FLAGS) != 0))
    {
        return nullptr;
    }

    NamedIntrinsic intrinsic;
    if (op1->TypeIs(TYP_LONG) && comp->compOpportunisticallyDependsOn(InstructionSet_AVX2_X64))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_X64_ResetLowestSetBit;
    }
    else if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_ResetLowestSetBit;
    }
    else
    {
        return nullptr;
    }

    LIR::Use use;
    if (!BlockRange().TryGetUse(andNode, &use))
    {
        return nullptr;
    }

    GenTreeHWIntrinsic* blsrNode = comp->gtNewScalarHWIntrinsicNode(andNode->TypeGet(), op1, intrinsic);

    JITDUMP("Lower: optimize AND(X, ADD(X, -1))\n");
    DISPNODE(andNode);
    JITDUMP("to:\n");
    DISPNODE(blsrNode);

    BlockRange().InsertBefore(andNode, blsrNode);
    use.ReplaceWith(blsrNode);

    BlockRange().Remove(andNode);
    BlockRange().Remove(op2);
    BlockRange().Remove(addOp1);
    BlockRange().Remove(addOp2);

    ContainCheckHWIntrinsic(blsrNode);

    return blsrNode;
}

//----------------------------------------------------------------------------------------------
// Lowering::TryLowerAndOpToExtractLowestSetIsolatedBit: Lowers a tree AND(X, NEG(X)) to
// HWIntrinsic::ExtractLowestSetBit
//
// Arguments:
//    andNode - GT_AND node of integral type
//
// Return Value:
//    Returns the replacement node if one is created else nullptr indicating no replacement
//
// Notes:
//    Performs containment checks on the replacement node if one is created
GenTree* Lowering::TryLowerAndOpToExtractLowestSetBit(GenTreeOp* andNode)
{
    GenTree* opNode  = nullptr;
    GenTree* negNode = nullptr;
    if (andNode->gtGetOp1()->OperIs(GT_NEG))
    {
        negNode = andNode->gtGetOp1();
        opNode  = andNode->gtGetOp2();
    }
    else if (andNode->gtGetOp2()->OperIs(GT_NEG))
    {
        negNode = andNode->gtGetOp2();
        opNode  = andNode->gtGetOp1();
    }

    if (opNode == nullptr)
    {
        return nullptr;
    }

    GenTree* negOp = negNode->AsUnOp()->gtGetOp1();
    if (!negOp->OperIs(GT_LCL_VAR) || !opNode->OperIs(GT_LCL_VAR) ||
        (negOp->AsLclVar()->GetLclNum() != opNode->AsLclVar()->GetLclNum()))
    {
        return nullptr;
    }

    // Subsequent nodes may rely on CPU flags set by these nodes in which case we cannot remove them
    if (((opNode->gtFlags & GTF_SET_FLAGS) != 0) || ((negNode->gtFlags & GTF_SET_FLAGS) != 0))
    {
        return nullptr;
    }

    NamedIntrinsic intrinsic;
    if (andNode->TypeIs(TYP_LONG) && comp->compOpportunisticallyDependsOn(InstructionSet_AVX2_X64))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_X64_ExtractLowestSetBit;
    }
    else if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_ExtractLowestSetBit;
    }
    else
    {
        return nullptr;
    }

    LIR::Use use;
    if (!BlockRange().TryGetUse(andNode, &use))
    {
        return nullptr;
    }

    GenTreeHWIntrinsic* blsiNode = comp->gtNewScalarHWIntrinsicNode(andNode->TypeGet(), opNode, intrinsic);

    JITDUMP("Lower: optimize AND(X, NEG(X)))\n");
    DISPNODE(andNode);
    JITDUMP("to:\n");
    DISPNODE(blsiNode);

    BlockRange().InsertBefore(andNode, blsiNode);
    use.ReplaceWith(blsiNode);

    BlockRange().Remove(andNode);
    BlockRange().Remove(negNode);
    BlockRange().Remove(negOp);

    ContainCheckHWIntrinsic(blsiNode);

    return blsiNode;
}

//----------------------------------------------------------------------------------------------
// Lowering::TryLowerAndOpToAndNot: Lowers a tree AND(X, NOT(Y)) to HWIntrinsic::AndNot
//
// Arguments:
//    andNode - GT_AND node of integral type
//
// Return Value:
//    Returns the replacement node if one is created else nullptr indicating no replacement
//
// Notes:
//    Performs containment checks on the replacement node if one is created
GenTree* Lowering::TryLowerAndOpToAndNot(GenTreeOp* andNode)
{
    assert(andNode->OperIs(GT_AND) && varTypeIsIntegral(andNode));

    GenTree* opNode  = nullptr;
    GenTree* notNode = nullptr;
    if (andNode->gtGetOp1()->OperIs(GT_NOT))
    {
        notNode = andNode->gtGetOp1();
        opNode  = andNode->gtGetOp2();
    }
    else if (andNode->gtGetOp2()->OperIs(GT_NOT))
    {
        notNode = andNode->gtGetOp2();
        opNode  = andNode->gtGetOp1();
    }

    if (opNode == nullptr)
    {
        return nullptr;
    }

    // We want to avoid using "andn" when one of the operands is both a source and the destination and is also coming
    // from memory. In this scenario, we will get smaller and likely faster code by using the RMW encoding of `and`
    if (IsBinOpInRMWStoreInd(andNode))
    {
        return nullptr;
    }

    // Subsequent nodes may rely on CPU flags set by these nodes in which case we cannot remove them
    if (((andNode->gtFlags & GTF_SET_FLAGS) != 0) || ((notNode->gtFlags & GTF_SET_FLAGS) != 0))
    {
        return nullptr;
    }

    NamedIntrinsic intrinsic;
    if (andNode->TypeIs(TYP_LONG) && comp->compOpportunisticallyDependsOn(InstructionSet_AVX2_X64))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_X64_AndNot;
    }
    else if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_AndNotScalar;
    }
    else
    {
        return nullptr;
    }

    LIR::Use use;
    if (!BlockRange().TryGetUse(andNode, &use))
    {
        return nullptr;
    }

    // note that parameter order for andn is ~y, x so these are purposefully reversed when creating the node
    GenTreeHWIntrinsic* andnNode =
        comp->gtNewScalarHWIntrinsicNode(andNode->TypeGet(), notNode->AsUnOp()->gtGetOp1(), opNode, intrinsic);

    JITDUMP("Lower: optimize AND(X, NOT(Y)))\n");
    DISPNODE(andNode);
    JITDUMP("to:\n");
    DISPNODE(andnNode);

    BlockRange().InsertBefore(andNode, andnNode);
    use.ReplaceWith(andnNode);

    BlockRange().Remove(andNode);
    BlockRange().Remove(notNode);

    ContainCheckHWIntrinsic(andnNode);

    return andnNode;
}

//----------------------------------------------------------------------------------------------
// Lowering::TryLowerXorOpToGetMaskUpToLowestSetBit: Lowers a tree XOR(X, ADD(X, -1)) to
// HWIntrinsic::GetMaskUpToLowestSetBit
//
// Arguments:
//    xorNode - GT_XOR node of integral type
//
// Return Value:
//    Returns the replacement node if one is created else nullptr indicating no replacement
//
// Notes:
//    Performs containment checks on the replacement node if one is created
GenTree* Lowering::TryLowerXorOpToGetMaskUpToLowestSetBit(GenTreeOp* xorNode)
{
    assert(xorNode->OperIs(GT_XOR) && varTypeIsIntegral(xorNode));

    GenTree* op1 = xorNode->gtGetOp1();
    if (!op1->OperIs(GT_LCL_VAR) || comp->lvaGetDesc(op1->AsLclVar())->IsAddressExposed())
    {
        return nullptr;
    }

    GenTree* op2 = xorNode->gtGetOp2();
    if (!op2->OperIs(GT_ADD))
    {
        return nullptr;
    }

    GenTree* addOp2 = op2->gtGetOp2();
    if (!addOp2->IsIntegralConst(-1))
    {
        return nullptr;
    }

    GenTree* addOp1 = op2->gtGetOp1();
    if (!addOp1->OperIs(GT_LCL_VAR) || (addOp1->AsLclVar()->GetLclNum() != op1->AsLclVar()->GetLclNum()))
    {
        return nullptr;
    }

    // Subsequent nodes may rely on CPU flags set by these nodes in which case we cannot remove them
    if (((addOp2->gtFlags & GTF_SET_FLAGS) != 0) || ((op2->gtFlags & GTF_SET_FLAGS) != 0) ||
        ((xorNode->gtFlags & GTF_SET_FLAGS) != 0))
    {
        return nullptr;
    }

    NamedIntrinsic intrinsic;
    if (xorNode->TypeIs(TYP_LONG) && comp->compOpportunisticallyDependsOn(InstructionSet_AVX2_X64))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_X64_GetMaskUpToLowestSetBit;
    }
    else if (comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
    {
        intrinsic = NamedIntrinsic::NI_AVX2_GetMaskUpToLowestSetBit;
    }
    else
    {
        return nullptr;
    }

    LIR::Use use;
    if (!BlockRange().TryGetUse(xorNode, &use))
    {
        return nullptr;
    }

    GenTreeHWIntrinsic* blsmskNode = comp->gtNewScalarHWIntrinsicNode(xorNode->TypeGet(), op1, intrinsic);

    JITDUMP("Lower: optimize XOR(X, ADD(X, -1)))\n");
    DISPNODE(xorNode);
    JITDUMP("to:\n");
    DISPNODE(blsmskNode);

    BlockRange().InsertBefore(xorNode, blsmskNode);
    use.ReplaceWith(blsmskNode);

    BlockRange().Remove(xorNode);
    BlockRange().Remove(op2);
    BlockRange().Remove(addOp1);
    BlockRange().Remove(addOp2);

    ContainCheckHWIntrinsic(blsmskNode);

    return blsmskNode;
}

//----------------------------------------------------------------------------------------------
// Lowering::LowerBswapOp: Tries to contain GT_BSWAP node when possible
//
// Arguments:
//    node - GT_BSWAP node to contain
//
// Notes:
//    Containment is not performed when optimizations are disabled
//    or when MOVBE instruction set is not found
//
void Lowering::LowerBswapOp(GenTreeOp* node)
{
    assert(node->OperIs(GT_BSWAP, GT_BSWAP16));

    if (!comp->opts.OptimizationEnabled() || !comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
    {
        return;
    }

    GenTree* operand  = node->gtGetOp1();
    unsigned swapSize = node->OperIs(GT_BSWAP16) ? 2 : genTypeSize(node);
    if ((swapSize == genTypeSize(operand)) && IsContainableMemoryOp(operand) && IsSafeToContainMem(node, operand))
    {
        MakeSrcContained(node, operand);
    }
}

#endif // FEATURE_HW_INTRINSICS

//----------------------------------------------------------------------------------------------
// Lowering::IsRMWIndirCandidate:
//    Returns true if the given operand is a candidate indirection for a read-modify-write
//    operator.
//
//  Arguments:
//     operand - The operand to consider.
//     storeInd - The indirect store that roots the possible RMW operator.
//
bool Lowering::IsRMWIndirCandidate(GenTree* operand, GenTree* storeInd)
{
    // If the operand isn't an indirection, it's trivially not a candidate.
    if (!operand->OperIs(GT_IND))
    {
        return false;
    }

    // If the indirection's source address isn't equivalent to the destination address of the storeIndir, then the
    // indirection is not a candidate.
    GenTree* srcAddr = operand->gtGetOp1();
    GenTree* dstAddr = storeInd->gtGetOp1();
    if ((srcAddr->OperGet() != dstAddr->OperGet()) || !IndirsAreEquivalent(operand, storeInd))
    {
        return false;
    }

    // If it is not safe to contain the entire tree rooted at the indirection, then the indirection is not a
    // candidate. Crawl the IR from the node immediately preceding the storeIndir until the last node in the
    // indirection's tree is visited and check the side effects at each point.

    m_scratchSideEffects.Clear();

    assert((operand->gtLIRFlags & LIR::Flags::Mark) == 0);
    operand->gtLIRFlags |= LIR::Flags::Mark;

    unsigned markCount = 1;
    GenTree* node;
    for (node = storeInd->gtPrev; markCount > 0; node = node->gtPrev)
    {
        assert(node != nullptr);

        if ((node->gtLIRFlags & LIR::Flags::Mark) == 0)
        {
            m_scratchSideEffects.AddNode(comp, node);
        }
        else
        {
            node->gtLIRFlags &= ~LIR::Flags::Mark;
            markCount--;

            if (m_scratchSideEffects.InterferesWith(comp, node, false))
            {
                // The indirection's tree contains some node that can't be moved to the storeInder. The indirection is
                // not a candidate. Clear any leftover mark bits and return.
                for (; markCount > 0; node = node->gtPrev)
                {
                    if ((node->gtLIRFlags & LIR::Flags::Mark) != 0)
                    {
                        node->gtLIRFlags &= ~LIR::Flags::Mark;
                        markCount--;
                    }
                }
                return false;
            }

            node->VisitOperands([&markCount](GenTree* nodeOperand) -> GenTree::VisitResult {
                assert((nodeOperand->gtLIRFlags & LIR::Flags::Mark) == 0);
                nodeOperand->gtLIRFlags |= LIR::Flags::Mark;
                markCount++;
                return GenTree::VisitResult::Continue;
            });
        }
    }

    // At this point we've verified that the operand is an indirection, its address is equivalent to the storeIndir's
    // destination address, and that it and the transitive closure of its operand can be safely contained by the
    // storeIndir. This indirection is therefore a candidate for an RMW op.
    return true;
}

//----------------------------------------------------------------------------------------------
// Returns true if this tree is bin-op of a GT_STOREIND of the following form
//      storeInd(subTreeA, binOp(gtInd(subTreeA), subtreeB)) or
//      storeInd(subTreeA, binOp(subtreeB, gtInd(subTreeA)) in case of commutative bin-ops
//
// The above form for storeInd represents a read-modify-write memory binary operation.
//
// Parameters
//     tree   -   GentreePtr of binOp
//
// Return Value
//     True if 'tree' is part of a RMW memory operation pattern
//
bool Lowering::IsBinOpInRMWStoreInd(GenTree* tree)
{
    // Must be a non floating-point type binary operator since SSE2 doesn't support RMW memory ops
    assert(!varTypeIsFloating(tree));
    assert(GenTree::OperIsBinary(tree->OperGet()));

    // Cheap bail out check before more expensive checks are performed.
    // RMW memory op pattern requires that one of the operands of binOp to be GT_IND.
    if (tree->gtGetOp1()->OperGet() != GT_IND && tree->gtGetOp2()->OperGet() != GT_IND)
    {
        return false;
    }

    LIR::Use use;
    if (!BlockRange().TryGetUse(tree, &use) || use.User()->OperGet() != GT_STOREIND || use.User()->gtGetOp2() != tree)
    {
        return false;
    }

    // Since it is not relatively cheap to recognize RMW memory op pattern, we
    // cache the result in GT_STOREIND node so that while lowering GT_STOREIND
    // we can use the result.
    GenTree* indirCandidate = nullptr;
    GenTree* indirOpSource  = nullptr;
    return IsRMWMemOpRootedAtStoreInd(use.User(), &indirCandidate, &indirOpSource);
}

//----------------------------------------------------------------------------------------------
// This method recognizes the case where we have a treeNode with the following structure:
//         storeInd(IndirDst, binOp(gtInd(IndirDst), indirOpSource)) OR
//         storeInd(IndirDst, binOp(indirOpSource, gtInd(IndirDst)) in case of commutative operations OR
//         storeInd(IndirDst, unaryOp(gtInd(IndirDst)) in case of unary operations
//
// Terminology:
//         indirDst = memory write of an addr mode  (i.e. storeind destination)
//         indirSrc = value being written to memory (i.e. storeind source which could either be a binary or unary op)
//         indirCandidate = memory read i.e. a gtInd of an addr mode
//         indirOpSource = source operand used in binary/unary op (i.e. source operand of indirSrc node)
//
// In x86/x64 this storeInd pattern can be effectively encoded in a single instruction of the
// following form in case of integer operations:
//         binOp [addressing mode], RegIndirOpSource
//         binOp [addressing mode], immediateVal
// where RegIndirOpSource is the register where indirOpSource was computed.
//
// Right now, we recognize few cases:
//     a) The gtInd child is a lea/lclVar/lclVarAddr/constant
//     b) BinOp is either add, sub, xor, or, and, shl, rsh, rsz.
//     c) unaryOp is either not/neg
//
// Implementation Note: The following routines need to be in sync for RMW memory op optimization
// to be correct and functional.
//     IndirsAreEquivalent()
//     NodesAreEquivalentLeaves()
//     Codegen of GT_STOREIND and genCodeForShiftRMW()
//     emitInsRMW()
//
//  TODO-CQ: Enable support for more complex indirections (if needed) or use the value numbering
//  package to perform more complex tree recognition.
//
//  TODO-XArch-CQ: Add support for RMW of lcl fields (e.g. lclfield binop= source)
//
//  Parameters:
//     tree               -  GT_STOREIND node
//     outIndirCandidate  -  out param set to indirCandidate as described above
//     ouutIndirOpSource  -  out param set to indirOpSource as described above
//
//  Return value
//     True if there is a RMW memory operation rooted at a GT_STOREIND tree
//     and out params indirCandidate and indirOpSource are set to non-null values.
//     Otherwise, returns false with indirCandidate and indirOpSource set to null.
//     Also updates flags of GT_STOREIND tree with its RMW status.
//
bool Lowering::IsRMWMemOpRootedAtStoreInd(GenTree* tree, GenTree** outIndirCandidate, GenTree** outIndirOpSource)
{
    assert(!varTypeIsFloating(tree));
    assert(outIndirCandidate != nullptr);
    assert(outIndirOpSource != nullptr);

    *outIndirCandidate = nullptr;
    *outIndirOpSource  = nullptr;

    // Early out if storeInd is already known to be a non-RMW memory op
    GenTreeStoreInd* storeInd = tree->AsStoreInd();
    if (storeInd->IsNonRMWMemoryOp())
    {
        return false;
    }

    GenTree*   indirDst = storeInd->gtGetOp1();
    GenTree*   indirSrc = storeInd->gtGetOp2();
    genTreeOps oper     = indirSrc->OperGet();

    // Early out if it is already known to be a RMW memory op
    if (storeInd->IsRMWMemoryOp())
    {
        if (GenTree::OperIsBinary(oper))
        {
            if (storeInd->IsRMWDstOp1())
            {
                *outIndirCandidate = indirSrc->gtGetOp1();
                *outIndirOpSource  = indirSrc->gtGetOp2();
            }
            else
            {
                assert(storeInd->IsRMWDstOp2());
                *outIndirCandidate = indirSrc->gtGetOp2();
                *outIndirOpSource  = indirSrc->gtGetOp1();
            }
            assert(IndirsAreEquivalent(*outIndirCandidate, storeInd));
        }
        else
        {
            assert(GenTree::OperIsUnary(oper));
            assert(IndirsAreEquivalent(indirSrc->gtGetOp1(), storeInd));
            *outIndirCandidate = indirSrc->gtGetOp1();
            *outIndirOpSource  = indirSrc->gtGetOp1();
        }

        return true;
    }

    // If reached here means that we do not know RMW status of tree rooted at storeInd
    assert(storeInd->IsRMWStatusUnknown());

    // Early out if indirDst is not one of the supported memory operands.
    if (!indirDst->OperIs(GT_LEA, GT_LCL_VAR, GT_CNS_INT) && !indirDst->IsLclVarAddr())
    {
        storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_ADDR);
        return false;
    }

    // We can not use Read-Modify-Write instruction forms with overflow checking instructions
    // because we are not allowed to modify the target until after the overflow check.
    if (indirSrc->gtOverflowEx())
    {
        storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_OPER);
        return false;
    }

    // At this point we can match one of two patterns:
    //
    //     t_ind = indir t_addr_0
    //       ...
    //     t_value = binop t_ind, t_other
    //       ...
    //     storeIndir t_addr_1, t_value
    //
    // or
    //
    //     t_ind = indir t_addr_0
    //       ...
    //     t_value = unop t_ind
    //       ...
    //     storeIndir t_addr_1, t_value
    //
    // In all cases, we will eventually make the binop that produces t_value and the entire dataflow tree rooted at
    // t_ind contained by t_value.

    GenTree*  indirCandidate = nullptr;
    GenTree*  indirOpSource  = nullptr;
    RMWStatus status         = STOREIND_RMW_STATUS_UNKNOWN;
    if (GenTree::OperIsBinary(oper))
    {
        // Return if binary op is not one of the supported operations for RMW of memory.
        if (!GenTree::OperIsRMWMemOp(oper))
        {
            storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_OPER);
            return false;
        }

        if (GenTree::OperIsShiftOrRotate(oper) && varTypeIsSmall(storeInd))
        {
            // In ldind, Integer values smaller than 4 bytes, a boolean, or a character converted to 4 bytes
            // by sign or zero-extension as appropriate. If we directly shift the short type data using sar, we
            // will lose the sign or zero-extension bits.
            storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_TYPE);
            return false;
        }

        // In the common case, the second operand to the binop will be the indir candidate.
        GenTreeOp* binOp = indirSrc->AsOp();
        if (GenTree::OperIsCommutative(oper) && IsRMWIndirCandidate(binOp->gtOp2, storeInd))
        {
            indirCandidate = binOp->gtOp2;
            indirOpSource  = binOp->gtOp1;
            status         = STOREIND_RMW_DST_IS_OP2;
        }
        else if (IsRMWIndirCandidate(binOp->gtOp1, storeInd))
        {
            indirCandidate = binOp->gtOp1;
            indirOpSource  = binOp->gtOp2;
            status         = STOREIND_RMW_DST_IS_OP1;
        }
        else
        {
            storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_ADDR);
            return false;
        }
    }
    else if (GenTree::OperIsUnary(oper))
    {
        // Nodes other than GT_NOT and GT_NEG are not yet supported.
        if (oper != GT_NOT && oper != GT_NEG)
        {
            storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_OPER);
            return false;
        }

        if (indirSrc->gtGetOp1()->OperGet() != GT_IND)
        {
            storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_ADDR);
            return false;
        }

        GenTreeUnOp* unOp = indirSrc->AsUnOp();
        if (IsRMWIndirCandidate(unOp->gtOp1, storeInd))
        {
            // src and dest are the same in case of unary ops
            indirCandidate = unOp->gtOp1;
            indirOpSource  = unOp->gtOp1;
            status         = STOREIND_RMW_DST_IS_OP1;
        }
        else
        {
            storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_ADDR);
            return false;
        }
    }
    else
    {
        storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_OPER);
        return false;
    }

    // By this point we've verified that we have a supported operand with a supported address. Now we need to ensure
    // that we're able to move the destination address for the source indirection forwards.
    if (!IsSafeToContainMem(storeInd, indirDst))
    {
        storeInd->SetRMWStatus(STOREIND_RMW_UNSUPPORTED_ADDR);
        return false;
    }

    assert(indirCandidate != nullptr);
    assert(indirOpSource != nullptr);
    assert(status != STOREIND_RMW_STATUS_UNKNOWN);

    *outIndirCandidate = indirCandidate;
    *outIndirOpSource  = indirOpSource;
    storeInd->SetRMWStatus(status);
    return true;
}

// anything is in range for AMD64
bool Lowering::IsCallTargetInRange(void* addr)
{
    return true;
}

// return true if the immediate can be folded into an instruction, for example small enough and non-relocatable
bool Lowering::IsContainableImmed(GenTree* parentNode, GenTree* childNode) const
{
    if (!childNode->IsIntCnsFitsInI32())
    {
        return false;
    }

    // At this point we know that it is an int const fits within 4-bytes and hence can safely cast to IntConCommon.
    // Icons that need relocation should never be marked as contained immed
    if (childNode->AsIntConCommon()->ImmedValNeedsReloc(comp))
    {
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------
// PreferredRegOptionalOperand: returns one of the operands of given
// binary oper that is to be preferred for marking as reg optional.
//
// Since only one of op1 or op2 can be a memory operand on xarch, only
// one of  them have to be marked as reg optional.  Since Lower doesn't
// know apriori which of op1 or op2 is not likely to get a register, it
// has to make a guess. This routine encapsulates heuristics that
// guess whether it is likely to be beneficial to mark op1 or op2 as
// reg optional.
//
//
// Arguments:
//     op1  - The first operand of the binary operation to consider
//     op2  - The second operand of the binary operation to consider
//
// Returns:
//     Returns op1 or op2 of tree node that is preferred for
//     marking as reg optional.
//
// Note: if the tree oper is neither commutative nor a compare oper
// then only op2 can be reg optional on xarch and hence no need to
// call this routine.
GenTree* Lowering::PreferredRegOptionalOperand(GenTree* op1, GenTree* op2)
{
    // This routine uses the following heuristics:
    //
    // a) If both are register candidates, marking the one with lower weighted
    // ref count as reg-optional would likely be beneficial as it has
    // higher probability of not getting a register. Note that we use !lvDoNotEnregister
    // here because this is being done while we are adding lclVars for Lowering.
    //
    // b) op1 = tracked local and op2 = untracked local: LSRA creates two
    // ref positions for op2: a def and use position. op2's def position
    // requires a reg and it is allocated a reg by spilling another
    // interval (if required) and that could be even op1.  For this reason
    // it is beneficial to mark op1 as reg optional.
    //
    // TODO: It is not always mandatory for a def position of an untracked
    // local to be allocated a register if it is on rhs of an assignment
    // and its use position is reg-optional and has not been assigned a
    // register.  Reg optional def positions is currently not yet supported.
    //
    // c) op1 = untracked local and op2 = tracked local: marking op1 as
    // reg optional is beneficial, since its use position is less likely
    // to get a register.
    //
    // d) If both are untracked locals (i.e. treated like tree temps by
    // LSRA): though either of them could be marked as reg optional,
    // marking op1 as reg optional is likely to be beneficial because
    // while allocating op2's def position, there is a possibility of
    // spilling op1's def and in which case op1 is treated as contained
    // memory operand rather than requiring to reload.
    //
    // e) If only one of them is a local var, prefer to mark it as
    // reg-optional.  This is heuristic is based on the results
    // obtained against CQ perf benchmarks.
    //
    // f) If neither of them are local vars (i.e. tree temps), prefer to
    // mark op1 as reg optional for the same reason as mentioned in (d) above.

    if (op1 == nullptr)
    {
        return op2;
    }

    assert(!op1->IsRegOptional());
    assert(!op2->IsRegOptional());

    // We default to op1, as op2 is likely to have the shorter lifetime.
    GenTree* preferredOp = op1;

    if (op1->OperIs(GT_LCL_VAR))
    {
        if (op2->OperIs(GT_LCL_VAR))
        {
            LclVarDsc* v1 = comp->lvaGetDesc(op1->AsLclVarCommon());
            LclVarDsc* v2 = comp->lvaGetDesc(op2->AsLclVarCommon());

            bool v1IsRegCandidate = !v1->lvDoNotEnregister;
            bool v2IsRegCandidate = !v2->lvDoNotEnregister;

            if (v1IsRegCandidate && v2IsRegCandidate)
            {
                // Both are enregisterable locals.  The one with lower weight is less likely
                // to get a register and hence beneficial to mark the one with lower
                // weight as reg optional.
                //
                // If either is not tracked, it may be that it was introduced after liveness
                // was run, in which case we will always prefer op1 (should we use raw refcnt??).

                if (v1->lvTracked && v2->lvTracked)
                {
                    if (v1->lvRefCntWtd() >= v2->lvRefCntWtd())
                    {
                        preferredOp = op2;
                    }
                }
            }
        }
    }
    else if (op2->OperIs(GT_LCL_VAR))
    {
        preferredOp = op2;
    }

    return preferredOp;
}

//------------------------------------------------------------------------
// Containment analysis
//------------------------------------------------------------------------

//------------------------------------------------------------------------
// ContainCheckCallOperands: Determine whether operands of a call should be contained.
//
// Arguments:
//    call       - The call node of interest
//
// Return Value:
//    None.
//
void Lowering::ContainCheckCallOperands(GenTreeCall* call)
{
    GenTree* ctrlExpr = call->gtControlExpr;
    if (call->gtCallType == CT_INDIRECT)
    {
        // either gtControlExpr != null or gtCallAddr != null.
        // Both cannot be non-null at the same time.
        assert(ctrlExpr == nullptr);
        assert(call->gtCallAddr != nullptr);
        ctrlExpr = call->gtCallAddr;

#ifdef TARGET_X86
        // Fast tail calls aren't currently supported on x86, but if they ever are, the code
        // below that handles indirect VSD calls will need to be fixed.
        assert(!call->IsFastTailCall() || !call->IsVirtualStub());
#endif // TARGET_X86
    }

    // set reg requirements on call target represented as control sequence.
    if (ctrlExpr != nullptr)
    {
        // we should never see a gtControlExpr whose type is void.
        assert(!ctrlExpr->TypeIs(TYP_VOID));

#ifdef TARGET_X86
        // On x86, we need to generate a very specific pattern for indirect VSD calls:
        //
        //    3-byte nop
        //    call dword ptr [eax]
        //
        // Where EAX is also used as an argument to the stub dispatch helper. Make
        // sure that the call target address is computed into EAX in this case.
        if (call->IsVirtualStub() && (call->gtCallType == CT_INDIRECT) && !comp->IsTargetAbi(CORINFO_NATIVEAOT_ABI))
        {
            assert(ctrlExpr->isIndir());
            MakeSrcContained(call, ctrlExpr);
        }
        else
#endif // TARGET_X86
            if (ctrlExpr->isIndir())
            {
                // We may have cases where we have set a register target on the ctrlExpr, but if it
                // contained we must clear it.
                ctrlExpr->SetRegNum(REG_NA);
                MakeSrcContained(call, ctrlExpr);
            }
    }
}

//------------------------------------------------------------------------
// ContainCheckIndir: Determine whether operands of an indir should be contained.
//
// Arguments:
//    node       - The indirection node of interest
//
// Notes:
//    This is called for both store and load indirections. In the former case, it is assumed that
//    LowerStoreIndir() has already been called to check for RMW opportunities.
//
// Return Value:
//    None.
//
void Lowering::ContainCheckIndir(GenTreeIndir* node)
{
    GenTree* addr = node->Addr();

    // If this is the rhs of a block copy it will be handled when we handle the store.
    if (node->TypeIs(TYP_STRUCT))
    {
        return;
    }

    if ((node->gtFlags & GTF_IND_REQ_ADDR_IN_REG) != 0)
    {
        // The address of an indirection that requires its address in a reg.
        // Skip any further processing that might otherwise make it contained.
    }
    else if (addr->OperIs(GT_LCL_ADDR) && IsContainableLclAddr(addr->AsLclFld(), node->Size()))
    {
        // These nodes go into an addr mode:
        // - GT_LCL_ADDR is a stack addr mode.
        MakeSrcContained(node, addr);
    }
    else if (addr->IsCnsIntOrI())
    {
        GenTreeIntConCommon* icon = addr->AsIntConCommon();

#if defined(FEATURE_SIMD)
        if ((!addr->TypeIs(TYP_SIMD12) || !icon->ImmedValNeedsReloc(comp)) && icon->FitsInAddrBase(comp))
#else
        if (icon->FitsInAddrBase(comp))
#endif
        {
            // On x86, direct VSD is done via a relative branch, and in fact it MUST be contained.
            //
            // Noting we cannot contain relocatable constants for TYP_SIMD12 today. Doing so would
            // require more advanced changes to the emitter so we can correctly track the handle and
            // the 8-byte offset needed for the second load/store used to process the upper element.

            MakeSrcContained(node, addr);
        }
    }
    else if (addr->OperIs(GT_LEA) && IsInvariantInRange(addr, node))
    {
        MakeSrcContained(node, addr);
    }
}

//------------------------------------------------------------------------
// ContainCheckStoreIndir: determine whether the sources of a STOREIND node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckStoreIndir(GenTreeStoreInd* node)
{
    // If the source is a containable immediate, make it contained, unless it is
    // an int-size or larger store of zero to memory, because we can generate smaller code
    // by zeroing a register and then storing it.
    GenTree* src = node->Data();

    if (IsContainableImmed(node, src) && (!src->IsIntegralConst(0) || varTypeIsSmall(node)))
    {
        MakeSrcContained(node, src);
    }

    // If the source is a BSWAP, contain it on supported hardware to generate a MOVBE.
    if (comp->opts.OptimizationEnabled())
    {
        if (src->OperIs(GT_BSWAP, GT_BSWAP16) && comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
        {
            unsigned swapSize = src->OperIs(GT_BSWAP16) ? 2 : genTypeSize(src);

            if ((swapSize == genTypeSize(node)) && IsInvariantInRange(src, node))
            {
                // Prefer containing in the store in case the load has been contained.
                src->gtGetOp1()->ClearContained();

                MakeSrcContained(node, src);
            }
        }
#if defined(FEATURE_HW_INTRINSICS)
        else if (src->OperIsHWIntrinsic())
        {
            GenTreeHWIntrinsic* hwintrinsic        = src->AsHWIntrinsic();
            NamedIntrinsic      intrinsicId        = hwintrinsic->GetHWIntrinsicId();
            var_types           simdBaseType       = hwintrinsic->GetSimdBaseType();
            bool                isContainable      = false;
            GenTree*            clearContainedNode = nullptr;

            switch (intrinsicId)
            {
                case NI_Vector128_ToScalar:
                case NI_Vector256_ToScalar:
                case NI_Vector512_ToScalar:
                {
                    // These intrinsics are "ins reg/mem, xmm" or "ins xmm, reg/mem"
                    //
                    // In the case we are coming from and going to memory, we want to
                    // preserve the original containment as we'll end up emitting a pair
                    // of scalar moves. e.g. for float:
                    //    movss xmm0, [addr1]           ; Size: 4, Latency: 4-7,  TP: 0.5
                    //    movss [addr2], xmm0           ; Size: 4, Latency: 4-10, TP: 1
                    //
                    // However, we want to prefer containing the store over allowing the
                    // input to be regOptional, so track and clear containment if required.

                    GenTree* op1       = hwintrinsic->Op(1);
                    clearContainedNode = op1;
                    isContainable      = !clearContainedNode->isContained();

                    if (isContainable && varTypeIsIntegral(simdBaseType))
                    {
                        isContainable = (genTypeSize(simdBaseType) == genTypeSize(node)) &&
                                        (!varTypeIsSmall(simdBaseType) ||
                                         comp->compOpportunisticallyDependsOn(InstructionSet_SSE42));

                        if (isContainable && varTypeIsSmall(simdBaseType))
                        {
                            CorInfoType baseJitType = varTypeIsByte(node) ? CORINFO_TYPE_UBYTE : CORINFO_TYPE_USHORT;

                            if (intrinsicId == NI_Vector512_ToScalar)
                            {
                                op1 = comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, op1, NI_Vector512_GetLower128,
                                                                     baseJitType, 64);
                                BlockRange().InsertBefore(hwintrinsic, op1);
                                LowerNode(op1);
                            }
                            else if (intrinsicId == NI_Vector256_ToScalar)
                            {
                                op1 = comp->gtNewSimdGetLowerNode(TYP_SIMD16, op1, baseJitType, 32);
                                BlockRange().InsertBefore(hwintrinsic, op1);
                                LowerNode(op1);
                            }

                            intrinsicId = varTypeIsByte(node) ? NI_SSE42_Extract : NI_X86Base_Extract;

                            GenTree* zero = comp->gtNewZeroConNode(TYP_INT);
                            BlockRange().InsertBefore(hwintrinsic, zero);

                            hwintrinsic->SetSimdBaseJitType(baseJitType);
                            hwintrinsic->SetSimdSize(16);
                            hwintrinsic->ResetHWIntrinsicId(intrinsicId, op1, zero);
                            zero->SetContained();
                        }
                    }
                    break;
                }

                case NI_X86Base_ConvertToInt32:
                case NI_X86Base_ConvertToUInt32:
                case NI_X86Base_X64_ConvertToInt64:
                case NI_X86Base_X64_ConvertToUInt64:
                case NI_AVX2_ConvertToInt32:
                case NI_AVX2_ConvertToUInt32:
                {
                    // These intrinsics are "ins reg/mem, xmm"
                    isContainable = varTypeIsIntegral(simdBaseType) && (genTypeSize(src) == genTypeSize(node));
                    break;
                }

                case NI_Vector128_GetElement:
                {
                    // GetElement for floating-point is specially handled since double
                    // doesn't have a direct "extract" instruction and float cannot extract
                    // to a SIMD register.
                    //
                    // However, we still want to do the efficient thing and write directly
                    // to memory in the case where the extract is immediately used by a store

                    if (varTypeIsFloating(simdBaseType) && hwintrinsic->Op(2)->IsCnsIntOrI())
                    {
                        assert(!hwintrinsic->Op(2)->IsIntegralConst(0));

                        if (simdBaseType == TYP_FLOAT)
                        {
                            // SSE41.Extract is "extractps reg/mem, xmm, imm8"
                            //
                            // In the case we are coming from and going to memory, we want to
                            // preserve the original containment as we'll end up emitting:
                            //    movss xmm0, [addr1]           ; Size: 4, Latency: 4-7,  TP: 0.5
                            //    movss [addr2], xmm0           ; Size: 4, Latency: 4-10, TP: 1
                            //
                            // The alternative would be emitting the slightly more expensive
                            //    movups xmm0, [addr1]          ; Size: 4, Latency: 4-7,  TP: 0.5
                            //    extractps [addr2], xmm0, cns  ; Size: 6, Latency: 5-10, TP: 1
                            //
                            // However, we want to prefer containing the store over allowing the
                            // input to be regOptional, so track and clear containment if required.

                            if (comp->compOpportunisticallyDependsOn(InstructionSet_SSE42))
                            {
                                clearContainedNode = hwintrinsic->Op(1);
                                isContainable      = !clearContainedNode->isContained();
                            }
                        }
                        else
                        {
                            // TODO-XArch-CQ: We really should specially handle TYP_DOUBLE here but
                            // it requires handling GetElement(1) and GT_STOREIND as NI_X86Base_StoreHigh
                            assert(!isContainable);
                        }
                    }
                    break;
                }

                case NI_X86Base_Extract:
                case NI_SSE42_Extract:
                case NI_SSE42_X64_Extract:
                case NI_AVX_ExtractVector128:
                case NI_AVX2_ExtractVector128:
                case NI_AVX512_ExtractVector128:
                case NI_AVX512_ExtractVector256:
                {
                    // These intrinsics are "ins reg/mem, xmm, imm8"

                    size_t   numArgs = hwintrinsic->GetOperandCount();
                    GenTree* lastOp  = hwintrinsic->Op(numArgs);

                    isContainable = HWIntrinsicInfo::isImmOp(intrinsicId, lastOp) && lastOp->IsCnsIntOrI() &&
                                    (genTypeSize(simdBaseType) == genTypeSize(node));

                    if (isContainable && (intrinsicId == NI_X86Base_Extract))
                    {
                        // Validate the pextrw encoding supports containment
                        isContainable = comp->compOpportunisticallyDependsOn(InstructionSet_SSE42);
                    }
                    break;
                }

                case NI_AVX512_ConvertToVector128UInt32:
                case NI_AVX512_ConvertToVector128UInt32WithSaturation:
                case NI_AVX512_ConvertToVector256Int32:
                case NI_AVX512_ConvertToVector256UInt32:
                {
                    if (varTypeIsFloating(simdBaseType))
                    {
                        break;
                    }
                    FALLTHROUGH;
                }

                case NI_AVX512_ConvertToVector128Byte:
                case NI_AVX512_ConvertToVector128ByteWithSaturation:
                case NI_AVX512_ConvertToVector128Int16:
                case NI_AVX512_ConvertToVector128Int16WithSaturation:
                case NI_AVX512_ConvertToVector128Int32:
                case NI_AVX512_ConvertToVector128Int32WithSaturation:
                case NI_AVX512_ConvertToVector128SByte:
                case NI_AVX512_ConvertToVector128SByteWithSaturation:
                case NI_AVX512_ConvertToVector128UInt16:
                case NI_AVX512_ConvertToVector128UInt16WithSaturation:
                case NI_AVX512_ConvertToVector256Byte:
                case NI_AVX512_ConvertToVector256ByteWithSaturation:
                case NI_AVX512_ConvertToVector256Int16:
                case NI_AVX512_ConvertToVector256Int16WithSaturation:
                case NI_AVX512_ConvertToVector256Int32WithSaturation:
                case NI_AVX512_ConvertToVector256SByte:
                case NI_AVX512_ConvertToVector256SByteWithSaturation:
                case NI_AVX512_ConvertToVector256UInt16:
                case NI_AVX512_ConvertToVector256UInt16WithSaturation:
                case NI_AVX512_ConvertToVector256UInt32WithSaturation:
                {
                    // These intrinsics are "ins reg/mem, xmm"
                    instruction  ins       = HWIntrinsicInfo::lookupIns(intrinsicId, simdBaseType, comp);
                    insTupleType tupleType = emitter::insTupleTypeInfo(ins);
                    unsigned     simdSize  = hwintrinsic->GetSimdSize();
                    unsigned     memSize   = 0;

                    switch (tupleType)
                    {
                        case INS_TT_HALF_MEM:
                        {
                            memSize = simdSize / 2;
                            break;
                        }

                        case INS_TT_QUARTER_MEM:
                        {
                            memSize = simdSize / 4;
                            break;
                        }

                        case INS_TT_EIGHTH_MEM:
                        {
                            memSize = simdSize / 8;
                            break;
                        }

                        default:
                        {
                            unreached();
                        }
                    }

                    if (genTypeSize(node) == memSize)
                    {
                        isContainable = true;
                    }
                    break;
                }

                default:
                {
                    break;
                }
            }

            if (isContainable && IsInvariantInRange(src, node))
            {
                MakeSrcContained(node, src);

                if (clearContainedNode != nullptr)
                {
                    // Ensure we aren't marked contained or regOptional
                    clearContainedNode->ClearContained();
                }
            }
        }
#endif // FEATURE_HW_INTRINSICS
    }

    ContainCheckIndir(node);
}

//------------------------------------------------------------------------
// ContainCheckMul: determine whether the sources of a MUL node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckMul(GenTreeOp* node)
{
#if defined(TARGET_X86)
    assert(node->OperIs(GT_MUL, GT_MULHI, GT_MUL_LONG));
#else
    assert(node->OperIs(GT_MUL, GT_MULHI));
#endif

    // Case of float/double mul.
    if (varTypeIsFloating(node->TypeGet()))
    {
        ContainCheckFloatBinary(node);
        return;
    }

    GenTree* op1 = node->AsOp()->gtOp1;
    GenTree* op2 = node->AsOp()->gtOp2;

    bool isSafeToContainOp1 = true;
    bool isSafeToContainOp2 = true;

    bool     isUnsignedMultiply    = node->IsUnsigned();
    bool     requiresOverflowCheck = node->gtOverflowEx();
    bool     useLeaEncoding        = false;
    GenTree* memOp                 = nullptr;

    bool                 hasImpliedFirstOperand = false;
    GenTreeIntConCommon* imm                    = nullptr;
    GenTree*             other                  = nullptr;
    var_types            nodeType               = node->TypeGet();

    // Multiply should never be using small types
    assert(!varTypeIsSmall(node->TypeGet()));

    // We do use the widening multiply to implement
    // the overflow checking for unsigned multiply
    //
    if (isUnsignedMultiply && requiresOverflowCheck)
    {
        hasImpliedFirstOperand = true;
    }
    else if (node->OperIs(GT_MULHI))
    {
        hasImpliedFirstOperand = true;
    }
#if defined(TARGET_X86)
    else if (node->OperIs(GT_MUL_LONG))
    {
        hasImpliedFirstOperand = true;
        // GT_MUL_LONG hsa node type LONG but work on INT
        nodeType = TYP_INT;
    }
#endif
    else if (IsContainableImmed(node, op2) || IsContainableImmed(node, op1))
    {
        if (IsContainableImmed(node, op2))
        {
            imm   = op2->AsIntConCommon();
            other = op1;
        }
        else
        {
            imm   = op1->AsIntConCommon();
            other = op2;
        }

        // CQ: We want to rewrite this into a LEA
        ssize_t immVal = imm->AsIntConCommon()->IconValue();
        if (!requiresOverflowCheck && (immVal == 3 || immVal == 5 || immVal == 9))
        {
            useLeaEncoding = true;
        }

        MakeSrcContained(node, imm); // The imm is always contained
        if (IsContainableMemoryOp(other))
        {
            memOp = other; // memOp may be contained below
        }
    }

    // We allow one operand to be a contained memory operand.
    // The memory op type must match with the 'node' type.
    // This is because during codegen we use 'node' type to derive EmitTypeSize.
    // E.g op1 type = byte, op2 type = byte but GT_MUL node type is int.
    //
    if (memOp == nullptr)
    {
        if ((op2->TypeGet() == nodeType) && IsContainableMemoryOp(op2))
        {
            isSafeToContainOp2 = IsSafeToContainMem(node, op2);
            if (isSafeToContainOp2)
            {
                memOp = op2;
            }
        }

        if ((memOp == nullptr) && (op1->TypeGet() == nodeType) && IsContainableMemoryOp(op1))
        {
            isSafeToContainOp1 = IsSafeToContainMem(node, op1);
            if (isSafeToContainOp1)
            {
                memOp = op1;
            }
        }
    }
    else
    {
        if ((memOp->TypeGet() != nodeType))
        {
            memOp = nullptr;
        }
        else if (!IsSafeToContainMem(node, memOp))
        {
            if (memOp == op1)
            {
                isSafeToContainOp1 = false;
            }
            else
            {
                isSafeToContainOp2 = false;
            }
            memOp = nullptr;
        }
    }
    // To generate an LEA we need to force memOp into a register
    // so don't allow memOp to be 'contained'
    //
    if (!useLeaEncoding)
    {
        if (memOp != nullptr)
        {
            MakeSrcContained(node, memOp);
        }
        else
        {
            if (imm != nullptr)
            {
                // Has a contained immediate operand.
                // Only 'other' operand can be marked as reg optional.
                assert(other != nullptr);

                isSafeToContainOp1 = ((other == op1) && IsSafeToMarkRegOptional(node, op1));
                isSafeToContainOp2 = ((other == op2) && IsSafeToMarkRegOptional(node, op2));
            }
            else if (hasImpliedFirstOperand)
            {
                // Only op2 can be marked as reg optional.
                isSafeToContainOp1 = false;
                isSafeToContainOp2 = isSafeToContainOp2 && IsSafeToMarkRegOptional(node, op2);
            }
            else
            {
                // If there are no containable operands, we can make either of op1 or op2
                // as reg optional.
                isSafeToContainOp1 = isSafeToContainOp1 && IsSafeToMarkRegOptional(node, op1);
                isSafeToContainOp2 = isSafeToContainOp2 && IsSafeToMarkRegOptional(node, op2);
            }
            SetRegOptionalForBinOp(node, isSafeToContainOp1, isSafeToContainOp2);
        }
    }
}

//------------------------------------------------------------------------
// ContainCheckDivOrMod: determine which operands of a div/mod should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckDivOrMod(GenTreeOp* node)
{
    assert(node->OperIs(GT_DIV, GT_MOD, GT_UDIV, GT_UMOD));

    if (varTypeIsFloating(node->TypeGet()))
    {
        ContainCheckFloatBinary(node);
        return;
    }

    GenTree* divisor = node->gtGetOp2();

    bool divisorCanBeRegOptional = true;
#ifdef TARGET_X86
    GenTree* dividend = node->gtGetOp1();
    if (dividend->OperIs(GT_LONG))
    {
        divisorCanBeRegOptional = false;
        MakeSrcContained(node, dividend);
    }
#endif

    // divisor can be an r/m, but the memory indirection must be of the same size as the divide
    if (IsContainableMemoryOp(divisor) && (divisor->TypeGet() == node->TypeGet()) && IsInvariantInRange(divisor, node))
    {
        MakeSrcContained(node, divisor);
    }
    else if (divisorCanBeRegOptional && IsSafeToMarkRegOptional(node, divisor))
    {
        // If there are no containable operands, we can make an operand reg optional.
        // Div instruction allows only divisor to be a memory op.
        divisor->SetRegOptional();
    }
}

//------------------------------------------------------------------------
// ContainCheckShiftRotate: determine whether the sources of a shift/rotate node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckShiftRotate(GenTreeOp* node)
{
    assert(node->OperIsShiftOrRotate());

    GenTree* source  = node->gtOp1;
    GenTree* shiftBy = node->gtOp2;

#ifdef TARGET_X86
    if (node->OperIsShiftLong())
    {
        assert(source->OperIs(GT_LONG));
        MakeSrcContained(node, source);
    }
#endif // TARGET_X86

    if (IsContainableImmed(node, shiftBy) && (shiftBy->AsIntConCommon()->IconValue() <= 255) &&
        (shiftBy->AsIntConCommon()->IconValue() >= 0))
    {
        MakeSrcContained(node, shiftBy);
    }

    bool canContainSource = !source->isContained() && (genTypeSize(source) >= genTypeSize(node));

    // BMI2 rotate and shift instructions take memory operands but do not set flags.
    // rorx takes imm8 for the rotate amount; shlx/shrx/sarx take r32/64 for shift amount.
    if (canContainSource && !node->gtSetFlags() && (shiftBy->isContained() != node->OperIsShift()) &&
        comp->compOpportunisticallyDependsOn(InstructionSet_AVX2))
    {
        if (IsContainableMemoryOp(source) && IsSafeToContainMem(node, source))
        {
            MakeSrcContained(node, source);
        }
        else if (IsSafeToMarkRegOptional(node, source))
        {
            MakeSrcRegOptional(node, source);
        }
    }
}

//------------------------------------------------------------------------
// ContainCheckStoreLoc: determine whether the source of a STORE_LCL* should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckStoreLoc(GenTreeLclVarCommon* storeLoc) const
{
    assert(storeLoc->OperIsLocalStore());
    GenTree* op1 = storeLoc->gtGetOp1();

    if (op1->OperIs(GT_BITCAST))
    {
        // If we know that the source of the bitcast will be in a register, then we can make
        // the bitcast itself contained. This will allow us to store directly from the other
        // type if this node doesn't get a register.
        GenTree* bitCastSrc = op1->gtGetOp1();
        if (!bitCastSrc->isContained() && !bitCastSrc->IsRegOptional())
        {
            op1->SetContained();
            return;
        }
    }

    const LclVarDsc* varDsc = comp->lvaGetDesc(storeLoc);

#ifdef FEATURE_SIMD
    if (varTypeIsSIMD(storeLoc))
    {
        assert(!op1->IsCnsIntOrI());
        return;
    }
#endif // FEATURE_SIMD

    // If the source is a containable immediate, make it contained, unless it is
    // an int-size or larger store of zero to memory, because we can generate smaller code
    // by zeroing a register and then storing it.
    var_types type = varDsc->GetRegisterType(storeLoc);
    if (IsContainableImmed(storeLoc, op1) && (!op1->IsIntegralConst(0) || varTypeIsSmall(type)))
    {
        MakeSrcContained(storeLoc, op1);
    }
#ifdef TARGET_X86
    else if (op1->OperIs(GT_LONG))
    {
        MakeSrcContained(storeLoc, op1);
    }
#endif // TARGET_X86
}

//------------------------------------------------------------------------
// ContainCheckCast: determine whether the source of a CAST node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckCast(GenTreeCast* node)
{
    GenTree*  castOp     = node->CastOp();
    var_types castToType = node->CastToType();
    var_types srcType    = castOp->TypeGet();

    // force the srcType to unsigned if GT_UNSIGNED flag is set
    if (node->gtFlags & GTF_UNSIGNED)
    {
        srcType = varTypeToUnsigned(srcType);
    }

    if (!node->gtOverflow())
    {
        // Some casts will be able to use the source from memory.
        bool srcIsContainable = false;

        if (varTypeIsFloating(castToType) || varTypeIsFloating(srcType))
        {
            if (castOp->IsCnsNonZeroFltOrDbl())
            {
                MakeSrcContained(node, castOp);
            }
            else
            {
                // The ulong->floating SSE2 fallback requires the source to be in register
                srcIsContainable = !varTypeIsSmall(srcType) && ((srcType != TYP_ULONG) || comp->canUseEvexEncoding());
            }
        }
        else if (comp->opts.OptimizationEnabled() && varTypeIsIntegral(castOp) && varTypeIsIntegral(castToType))
        {
            // Most integral casts can be re-expressed as loads, except those that would be changing the sign.
            if (!varTypeIsSmall(castOp) || (varTypeIsUnsigned(castOp) == node->IsZeroExtending()))
            {
                srcIsContainable = true;
            }
        }

        if (srcIsContainable)
        {
            TryMakeSrcContainedOrRegOptional(node, castOp);
        }
    }

#if !defined(TARGET_64BIT)
    if (varTypeIsLong(srcType))
    {
        noway_assert(castOp->OperIs(GT_LONG));
        castOp->SetContained();
    }
#endif // !defined(TARGET_64BIT)
}

//------------------------------------------------------------------------
// ContainCheckCompare: determine whether the sources of a compare node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckCompare(GenTreeOp* cmp)
{
    assert(cmp->OperIsCompare() || cmp->OperIs(GT_CMP, GT_TEST));

    GenTree*  op1     = cmp->AsOp()->gtOp1;
    GenTree*  op2     = cmp->AsOp()->gtOp2;
    var_types op1Type = op1->TypeGet();
    var_types op2Type = op2->TypeGet();

    // If either of op1 or op2 is floating point values, then we need to use
    // ucomiss or ucomisd to compare, both of which support the following form:
    //     ucomis[s|d] xmm, xmm/mem
    // That is only the second operand can be a memory op.
    //
    // Second operand is a memory Op:  Note that depending on comparison operator,
    // the operands of ucomis[s|d] need to be reversed.  Therefore, either op1 or
    // op2 can be a memory op depending on the comparison operator.
    if (varTypeIsFloating(op1Type))
    {
        // The type of the operands has to be the same and no implicit conversions at this stage.
        assert(op1Type == op2Type);

        GenTree* otherOp;
        if (GenCondition::FromFloatRelop(cmp).PreferSwap())
        {
            otherOp = op1;
        }
        else
        {
            otherOp = op2;
        }

        assert(otherOp != nullptr);
        bool isSafeToContainOtherOp = true;
        if (otherOp->IsCnsNonZeroFltOrDbl())
        {
            MakeSrcContained(cmp, otherOp);
        }
        else if (IsContainableMemoryOp(otherOp))
        {
            isSafeToContainOtherOp = IsSafeToContainMem(cmp, otherOp);
            if (isSafeToContainOtherOp)
            {
                MakeSrcContained(cmp, otherOp);
            }
        }

        if (!otherOp->isContained() && IsSafeToMarkRegOptional(cmp, otherOp))
        {
            // SSE2 allows only otherOp to be a memory-op. Since otherOp is not
            // contained, we can mark it reg-optional.
            // IsSafeToContainMem is expensive so we call it at most once for otherOp.
            // If we already called IsSafeToContainMem, it must have returned false;
            // otherwise, otherOp would be contained.
            MakeSrcRegOptional(cmp, otherOp);
        }

        return;
    }

    // TODO-XArch-CQ: factor out cmp optimization in 'genCondSetFlags' to be used here
    // or in other backend.

    if (CheckImmedAndMakeContained(cmp, op2))
    {
        // If the types are the same, or if the constant is of the correct size,
        // we can treat the MemoryOp as contained.
        if (op1Type == op2Type)
        {
            TryMakeSrcContainedOrRegOptional(cmp, op1);
        }
    }
    else if (op1Type == op2Type)
    {
        // Note that TEST does not have a r,rm encoding like CMP has but we can still
        // contain the second operand because the emitter maps both r,rm and rm,r to
        // the same instruction code. This avoids the need to special case TEST here.

        bool isSafeToContainOp1 = true;
        bool isSafeToContainOp2 = true;

        if (IsContainableMemoryOp(op2))
        {
            isSafeToContainOp2 = IsSafeToContainMem(cmp, op2);
            if (isSafeToContainOp2)
            {
                MakeSrcContained(cmp, op2);
            }
        }

        if (!op2->isContained() && IsContainableMemoryOp(op1))
        {
            isSafeToContainOp1 = IsSafeToContainMem(cmp, op1);
            if (isSafeToContainOp1)
            {
                MakeSrcContained(cmp, op1);
            }
        }

        if (!op1->isContained() && !op2->isContained())
        {
            // One of op1 or op2 could be marked as reg optional
            // to indicate that codegen can still generate code
            // if one of them is on stack.
            GenTree* regOptionalCandidate = op1->IsCnsIntOrI() ? op2 : PreferredRegOptionalOperand(op1, op2);

            bool setRegOptional =
                (regOptionalCandidate == op1) ? IsSafeToMarkRegOptional(cmp, op1) : IsSafeToMarkRegOptional(cmp, op2);
            if (setRegOptional)
            {
                MakeSrcRegOptional(cmp, regOptionalCandidate);
            }
        }
    }
}

//------------------------------------------------------------------------
// ContainCheckSelect: determine whether the sources of a select should be contained.
//
// Arguments:
//    select - the GT_SELECT or GT_SELECTCC node.
//
void Lowering::ContainCheckSelect(GenTreeOp* select)
{
    assert(select->OperIs(GT_SELECT, GT_SELECTCC));

    if (select->OperIs(GT_SELECTCC))
    {
        GenCondition cc = select->AsOpCC()->gtCondition;

        // op1 and op2 are emitted as two separate instructions due to the
        // conditional nature of cmov, so both operands can usually be
        // contained memory operands. The exception is for compares
        // requiring two cmovs, in which case we do not want to incur the
        // memory access/address calculation twice.
        //
        // See the comment in Codegen::GenConditionDesc::map for why these
        // comparisons are special and end up requiring the two cmovs.
        //
        switch (cc.GetCode())
        {
            case GenCondition::FEQ:
            case GenCondition::FLT:
            case GenCondition::FLE:
            case GenCondition::FNEU:
            case GenCondition::FGEU:
            case GenCondition::FGTU:
                // Skip containment checking below.
                // TODO-CQ: We could allow one of the operands to be a
                // contained memory operand, but it requires updating LSRA
                // build to take it into account.
                return;
            default:
                break;
        }
    }

    GenTree* op1 = select->gtOp1;
    GenTree* op2 = select->gtOp2;

    unsigned operSize = genTypeSize(select);
    assert((operSize == 4) || (operSize == TARGET_POINTER_SIZE));

    if (genTypeSize(op1) == operSize)
    {
        if (IsContainableMemoryOp(op1) && IsSafeToContainMem(select, op1))
        {
            MakeSrcContained(select, op1);
        }
        else if (IsSafeToMarkRegOptional(select, op1))
        {
            MakeSrcRegOptional(select, op1);
        }
    }

    if (genTypeSize(op2) == operSize)
    {
        if (IsContainableMemoryOp(op2) && IsSafeToContainMem(select, op2))
        {
            MakeSrcContained(select, op2);
        }
        else if (IsSafeToMarkRegOptional(select, op2))
        {
            MakeSrcRegOptional(select, op2);
        }
    }
}

//------------------------------------------------------------------------
// LowerRMWMemOp: Determine if this is a valid RMW mem op, and if so lower it accordingly
//
// Arguments:
//    node       - The indirect store node (GT_STORE_IND) of interest
//
// Return Value:
//    Returns true if 'node' is a valid RMW mem op; false otherwise.
//
bool Lowering::LowerRMWMemOp(GenTreeIndir* storeInd)
{
    assert(storeInd->OperIs(GT_STOREIND));

    // SSE2 doesn't support RMW on float values
    assert(!varTypeIsFloating(storeInd));

    // Terminology:
    // indirDst = memory write of an addr mode  (i.e. storeind destination)
    // indirSrc = value being written to memory (i.e. storeind source which could a binary/unary op)
    // indirCandidate = memory read i.e. a gtInd of an addr mode
    // indirOpSource = source operand used in binary/unary op (i.e. source operand of indirSrc node)

    GenTree* indirCandidate = nullptr;
    GenTree* indirOpSource  = nullptr;

    if (!IsRMWMemOpRootedAtStoreInd(storeInd, &indirCandidate, &indirOpSource))
    {
        JITDUMP("Lower of StoreInd didn't mark the node as self contained for reason: %s\n",
                RMWStatusDescription(storeInd->AsStoreInd()->GetRMWStatus()));
        DISPTREERANGE(BlockRange(), storeInd);
        return false;
    }

    GenTree*   indirDst = storeInd->gtGetOp1();
    GenTree*   indirSrc = storeInd->gtGetOp2();
    genTreeOps oper     = indirSrc->OperGet();

    // At this point we have successfully detected a RMW memory op of one of the following forms
    //         storeInd(indirDst, indirSrc(indirCandidate, indirOpSource)) OR
    //         storeInd(indirDst, indirSrc(indirOpSource, indirCandidate) in case of commutative operations OR
    //         storeInd(indirDst, indirSrc(indirCandidate) in case of unary operations
    //
    // Here indirSrc = one of the supported binary or unary operation for RMW of memory
    //      indirCandidate = a GT_IND node
    //      indirCandidateChild = operand of GT_IND indirCandidate
    //
    // The logic below does the following
    //      Make indirOpSource contained.
    //      Make indirSrc contained.
    //      Make indirCandidate contained.
    //      Make indirCandidateChild contained.
    //      Make indirDst contained except when it is a GT_LCL_VAR or GT_CNS_INT that doesn't fit within addr
    //      base.
    //

    // We have already done containment analysis on the indirSrc op.
    // If any of its operands are marked regOptional, reset that now.
    indirSrc->AsOp()->gtOp1->ClearRegOptional();
    if (GenTree::OperIsBinary(oper))
    {
        // On Xarch RMW operations require the source to be an immediate or in a register.
        // Therefore, if we have previously marked the indirOpSource as contained while lowering
        // the binary node, we need to reset that now.
        if (IsContainableMemoryOp(indirOpSource))
        {
            indirOpSource->ClearContained();
        }
        indirSrc->AsOp()->gtOp2->ClearRegOptional();
        JITDUMP("Lower successfully detected an assignment of the form: *addrMode BinOp= source\n");
    }
    else
    {
        assert(GenTree::OperIsUnary(oper));
        JITDUMP("Lower successfully detected an assignment of the form: *addrMode = UnaryOp(*addrMode)\n");
    }
    DISPTREERANGE(BlockRange(), storeInd);

    indirSrc->SetContained();
    indirCandidate->SetContained();

    GenTree* indirCandidateChild = indirCandidate->gtGetOp1();
    indirCandidateChild->SetContained();

    if (indirCandidateChild->OperIs(GT_LEA))
    {
        GenTreeAddrMode* addrMode = indirCandidateChild->AsAddrMode();

        if (addrMode->HasBase())
        {
            assert(addrMode->Base()->OperIsLeaf());
            addrMode->Base()->SetContained();
        }

        if (addrMode->HasIndex())
        {
            assert(addrMode->Index()->OperIsLeaf());
            addrMode->Index()->SetContained();
        }

        indirDst->SetContained();
    }
    else
    {
        assert(indirCandidateChild->OperIs(GT_LCL_VAR, GT_CNS_INT) || indirCandidateChild->IsLclVarAddr());

        // If it is a GT_LCL_VAR, it still needs the reg to hold the address.
        // We would still need a reg for GT_CNS_INT if it doesn't fit within addressing mode base.
        if (indirCandidateChild->OperIs(GT_LCL_ADDR) &&
            IsContainableLclAddr(indirCandidateChild->AsLclFld(), storeInd->Size()))
        {
            indirDst->SetContained();
        }
        else if (indirCandidateChild->IsCnsIntOrI() && indirCandidateChild->AsIntConCommon()->FitsInAddrBase(comp))
        {
            indirDst->SetContained();
        }
    }
    return true;
}

//------------------------------------------------------------------------
// ContainCheckBinary: Determine whether a binary op's operands should be contained.
//
// Arguments:
//    node - the node we care about
//
void Lowering::ContainCheckBinary(GenTreeOp* node)
{
    assert(node->OperIsBinary());

    if (varTypeIsFloating(node))
    {
        assert(node->OperIs(GT_ADD, GT_SUB));
        ContainCheckFloatBinary(node);
        return;
    }

    GenTree* op1 = node->gtOp1;
    GenTree* op2 = node->gtOp2;

    // We can directly encode the second operand if it is either a containable constant or a memory-op.
    // In case of memory-op, we can encode it directly provided its type matches with 'tree' type.
    // This is because during codegen, type of 'tree' is used to determine emit Type size. If the types
    // do not match, they get normalized (i.e. sign/zero extended) on load into a register.
    bool     directlyEncodable  = false;
    bool     binOpInRMW         = false;
    GenTree* operand            = nullptr;
    bool     isSafeToContainOp1 = true;
    bool     isSafeToContainOp2 = true;

    if (IsContainableImmed(node, op2))
    {
        directlyEncodable = true;
        operand           = op2;
    }
    else
    {
        binOpInRMW = IsBinOpInRMWStoreInd(node);
        if (!binOpInRMW)
        {
            if (IsContainableMemoryOpSize(node, op2) && IsContainableMemoryOp(op2))
            {
                isSafeToContainOp2 = IsSafeToContainMem(node, op2);
                if (isSafeToContainOp2)
                {
                    directlyEncodable = true;
                    operand           = op2;
                }
            }

            if ((operand == nullptr) && node->OperIsCommutative())
            {
                // If it is safe, we can reverse the order of operands of commutative operations for efficient
                // codegen
                if (IsContainableImmed(node, op1))
                {
                    directlyEncodable = true;
                    operand           = op1;
                }
                else if (IsContainableMemoryOpSize(node, op1) && IsContainableMemoryOp(op1))
                {
                    isSafeToContainOp1 = IsSafeToContainMem(node, op1);
                    if (isSafeToContainOp1)
                    {
                        directlyEncodable = true;
                        operand           = op1;
                    }
                }
            }
        }
    }

    if (directlyEncodable)
    {
        assert(operand != nullptr);
        MakeSrcContained(node, operand);
    }
    else if (!binOpInRMW)
    {
        // If this binary op neither has contained operands, nor is a
        // Read-Modify-Write (RMW) operation, we can mark its operands
        // as reg optional.

        isSafeToContainOp1 = IsSafeToMarkRegOptional(node, op1);
        isSafeToContainOp2 = IsSafeToMarkRegOptional(node, op2);

        SetRegOptionalForBinOp(node, isSafeToContainOp1, isSafeToContainOp2);
    }
}

//------------------------------------------------------------------------
// ContainCheckBoundsChk: determine whether any source of a bounds check node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckBoundsChk(GenTreeBoundsChk* node)
{
    assert(node->OperIs(GT_BOUNDS_CHECK));
    GenTree* other;
    if (CheckImmedAndMakeContained(node, node->GetIndex()))
    {
        other = node->GetArrayLength();
    }
    else if (CheckImmedAndMakeContained(node, node->GetArrayLength()))
    {
        other = node->GetIndex();
    }
    else if (IsContainableMemoryOp(node->GetIndex()))
    {
        other = node->GetIndex();
    }
    else
    {
        other = node->GetArrayLength();
    }

    if (node->GetIndex()->TypeGet() == node->GetArrayLength()->TypeGet())
    {
        TryMakeSrcContainedOrRegOptional(node, other);
    }
}

//------------------------------------------------------------------------
// ContainCheckIntrinsic: determine whether the source of an INTRINSIC node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckIntrinsic(GenTreeOp* node)
{
    assert(node->OperIs(GT_INTRINSIC));

    NamedIntrinsic intrinsicName = node->AsIntrinsic()->gtIntrinsicName;

    if ((intrinsicName == NI_System_Math_Ceiling) || (intrinsicName == NI_System_Math_Floor) ||
        (intrinsicName == NI_System_Math_Truncate) || (intrinsicName == NI_System_Math_Round) ||
        (intrinsicName == NI_System_Math_Sqrt))
    {
        GenTree* op1 = node->gtGetOp1();

        if (op1->IsCnsNonZeroFltOrDbl())
        {
            MakeSrcContained(node, op1);
        }
        else
        {
            TryMakeSrcContainedOrRegOptional(node, op1);
        }
    }
}

#ifdef FEATURE_HW_INTRINSICS
//----------------------------------------------------------------------------------------------
// IsContainableHWIntrinsicOp: Determines whether a child node is containable for a given HWIntrinsic
//
//  Arguments:
//     [In]  parentNode          - The hardware intrinsic node which is the parent of 'childNode'
//     [In]  childNode           - The node to check if it can be contained by 'parentNode'
//     [Out] supportsRegOptional - On return, this will be true if 'parentNode' supports 'childNode' being regOptional;
//     otherwise, false.
//
// Return Value:
//    true if 'childNode' is a containable by 'parentNode'; otherwise, false.
//
bool Lowering::IsContainableHWIntrinsicOp(GenTreeHWIntrinsic* parentNode, GenTree* childNode, bool* supportsRegOptional)
{
    assert(parentNode != nullptr);
    assert(childNode != nullptr);
    assert(supportsRegOptional != nullptr);

    NamedIntrinsic      parentIntrinsicId = parentNode->GetHWIntrinsicId();
    var_types           parentBaseType    = parentNode->GetSimdBaseType();
    HWIntrinsicCategory category          = HWIntrinsicInfo::lookupCategory(parentIntrinsicId);

    // We shouldn't have called in here if parentNode doesn't support containment
    assert(HWIntrinsicInfo::SupportsContainment(parentIntrinsicId));

    // In general, we can mark the child regOptional as long as it is at least as large as the parent instruction's
    // memory operand size.
    //
    // In cases where we are going to mark an operand regOptional, we need to know whether the reg will be GP or SIMD.
    // We can only contain a SIMD child if a GP reg is not a possibility.

    unsigned expectedSize     = parentNode->GetSimdSize();
    unsigned operandSize      = genTypeSize(childNode);
    bool     supportsMemoryOp = true;
    bool     supportsSIMDLoad = true;

    switch (category)
    {
        case HW_Category_MemoryLoad:
        {
            // The operand is a pointer, so we can't check the load size. Assume it matches.
            assert(varTypeIsI(childNode));

            operandSize = expectedSize;
            break;
        }

        case HW_Category_SimpleSIMD:
        case HW_Category_IMM:
        {
            instruction  ins       = HWIntrinsicInfo::lookupIns(parentIntrinsicId, parentBaseType, comp);
            insTupleType tupleType = emitter::insTupleTypeInfo(ins);

            switch (parentIntrinsicId)
            {
                case NI_SSE42_ConvertToVector128Int16:
                case NI_SSE42_ConvertToVector128Int32:
                case NI_SSE42_ConvertToVector128Int64:
                case NI_AVX2_ConvertToVector256Int16:
                case NI_AVX2_ConvertToVector256Int32:
                case NI_AVX2_ConvertToVector256Int64:
                {
                    // These can have either pointer or vector operands. For the pointer case, we can't check
                    // size, so just assume it matches. Otherwise, do normal size check based on tuple type.
                    if (parentNode->OperIsMemoryLoad())
                    {
                        operandSize = expectedSize;
                        break;
                    }

                    goto SIZE_FROM_TUPLE_TYPE;
                }

                case NI_X86Base_ShiftLeftLogical128BitLane:
                case NI_X86Base_ShiftRightLogical128BitLane:
                case NI_AVX2_ShiftLeftLogical128BitLane:
                case NI_AVX2_ShiftRightLogical128BitLane:
                {
                    if (!comp->canUseEvexEncoding())
                    {
                        supportsMemoryOp = false;
                        break;
                    }

                    goto SIZE_FROM_TUPLE_TYPE;
                }

                case NI_X86Base_ShiftLeftLogical:
                case NI_X86Base_ShiftRightArithmetic:
                case NI_X86Base_ShiftRightLogical:
                case NI_AVX2_ShiftLeftLogical:
                case NI_AVX2_ShiftRightArithmetic:
                case NI_AVX2_ShiftRightLogical:
                case NI_AVX512_ShiftLeftLogical:
                case NI_AVX512_ShiftRightArithmetic:
                case NI_AVX512_ShiftRightLogical:
                {
                    assert((tupleType & INS_TT_MEM128) != 0);

                    // Shift amount (op2) can be either imm8 or vector. If vector, it will always be xmm/m128.
                    //
                    // Otherwise, when using EVEX encoding, we can contain a load for the input vector (op1),
                    // so size comes from the parent.

                    if (!HWIntrinsicInfo::isImmOp(parentIntrinsicId, parentNode->Op(2)))
                    {
                        tupleType    = static_cast<insTupleType>(INS_TT_MEM128);
                        expectedSize = genTypeSize(TYP_SIMD16);
                        break;
                    }
                    else
                    {
                        tupleType = static_cast<insTupleType>(tupleType & ~INS_TT_MEM128);

                        if (!comp->canUseEvexEncoding())
                        {
                            supportsMemoryOp = false;
                            break;
                        }
                    }

                    goto SIZE_FROM_TUPLE_TYPE;
                }

                case NI_X86Base_Insert:
                case NI_SSE42_Insert:
                case NI_SSE42_X64_Insert:
                {
                    // insertps op2 is xmm/m32. If xmm, the upper 2 bits of op3 (imm8) are used to select the element
                    // position from the source vector; if m32, the source element selection bits in the imm8 are
                    // ignored.
                    //
                    // We will allow containment only if the immediate is constant and the element selection bits are
                    // explicitly zero.

                    if (ins == INS_insertps)
                    {
                        supportsMemoryOp = false;

                        GenTree* op3 = parentNode->Op(3);
                        if (op3->IsCnsIntOrI())
                        {
                            ssize_t ival = op3->AsIntCon()->IconValue();
                            assert((ival >= 0) && (ival <= 255));

                            expectedSize     = genTypeSize(TYP_FLOAT);
                            supportsMemoryOp = (ival <= 0x3F);
                        }
                        break;
                    }

                    assert(varTypeIsIntegral(childNode->TypeGet()));

                    // These load a scalar, so if the base type is integral, it may be in a GP reg.
                    expectedSize     = genTypeSize(parentBaseType);
                    supportsSIMDLoad = false;
                    break;
                }

                default:
                SIZE_FROM_TUPLE_TYPE:
                {
                    switch (tupleType)
                    {
                        case INS_TT_NONE:
                        case INS_TT_FULL:
                        case INS_TT_FULL_MEM:
                        {
                            // full parent size required
                            break;
                        }

                        case INS_TT_HALF:
                        case INS_TT_HALF_MEM:
                        {
                            expectedSize /= 2;
                            break;
                        }

                        case INS_TT_QUARTER_MEM:
                        {
                            expectedSize /= 4;
                            break;
                        }

                        case INS_TT_EIGHTH_MEM:
                        {
                            expectedSize /= 8;
                            break;
                        }

                        case INS_TT_MOVDDUP:
                        {
                            if (expectedSize == genTypeSize(TYP_SIMD16))
                            {
                                expectedSize /= 2;
                            }
                            break;
                        }

                        case INS_TT_TUPLE1_FIXED:
                        case INS_TT_TUPLE1_SCALAR:
                        {
                            expectedSize = CodeGenInterface::instInputSize(ins);
                            break;
                        }

                        case INS_TT_TUPLE2:
                        {
                            expectedSize = CodeGenInterface::instInputSize(ins) * 2;
                            break;
                        }

                        case INS_TT_TUPLE4:
                        {
                            expectedSize = CodeGenInterface::instInputSize(ins) * 4;
                            break;
                        }

                        case INS_TT_TUPLE8:
                        {
                            expectedSize = CodeGenInterface::instInputSize(ins) * 8;
                            break;
                        }

                        default:
                        {
                            unreached();
                        }
                    }
                    break;
                }
            }
            break;
        }

        case HW_Category_SIMDScalar:
        {
            expectedSize = genTypeSize(parentBaseType);

            switch (parentIntrinsicId)
            {
                case NI_Vector128_CreateScalar:
                case NI_Vector256_CreateScalar:
                case NI_Vector512_CreateScalar:
                case NI_Vector128_CreateScalarUnsafe:
                case NI_Vector256_CreateScalarUnsafe:
                case NI_Vector512_CreateScalarUnsafe:
                {
                    // Integral scalar loads to vector use movd/movq, so small types must be sized up.
                    // They may also use a GR reg, so disable SIMD operand containment.
                    if (varTypeIsIntegral(childNode->TypeGet()))
                    {
                        expectedSize     = genTypeSize(genActualType(parentBaseType));
                        supportsSIMDLoad = false;
                    }
                    break;
                }

                case NI_AVX2_BroadcastScalarToVector128:
                case NI_AVX2_BroadcastScalarToVector256:
                case NI_AVX512_BroadcastScalarToVector512:
                {
                    // These can have either pointer or vector operands. For the pointer case, we can't check
                    // size, so just assume it matches.
                    if (parentNode->OperIsMemoryLoad())
                    {
                        operandSize = expectedSize;
                        break;
                    }

                    // The vector case that supports containment is the
                    // BroadcastScalarToVector(CreateScalarUnsafe()) pattern.
                    break;
                }

                default:
                {
                    // Scalar integral values may be in a GP reg.
                    supportsSIMDLoad = !varTypeIsIntegral(childNode->TypeGet());
                    break;
                }
            }
            break;
        }

        case HW_Category_Scalar:
        {
            // We should only get here for integral nodes.
            assert(varTypeIsIntegral(childNode->TypeGet()));

            expectedSize = genTypeSize(parentNode);

            // CRC32 codegen depends on its second operand's type.
            // Currently, we are using SIMDBaseType to store the op2Type info.
            if (parentIntrinsicId == NI_SSE42_Crc32)
            {
                expectedSize = genTypeSize(parentBaseType);
            }

            break;
        }

        default:
        {
            unreached();
        }
    }

    supportsMemoryOp = supportsMemoryOp && (operandSize >= expectedSize);
    supportsSIMDLoad = supportsSIMDLoad && supportsMemoryOp;

    // SIMD16 loads are containable on non-VEX hardware only if the address is aligned.
    bool supportsUnalignedLoad = (expectedSize < genTypeSize(TYP_SIMD16)) || comp->canUseVexEncoding();
    *supportsRegOptional = supportsMemoryOp && supportsUnalignedLoad && IsSafeToMarkRegOptional(parentNode, childNode);

    if (!childNode->OperIsHWIntrinsic())
    {
        bool canBeContained = false;

        if (supportsMemoryOp)
        {
            if (IsContainableMemoryOp(childNode))
            {
                canBeContained = supportsUnalignedLoad && IsSafeToContainMem(parentNode, childNode);
            }
            else if (childNode->IsCnsNonZeroFltOrDbl())
            {
                // Always safe.
                canBeContained = true;
            }
            else if (childNode->IsCnsVec())
            {
                GenTreeVecCon* vecCon = childNode->AsVecCon();
                canBeContained        = !vecCon->IsAllBitsSet() && !vecCon->IsZero();
            }
        }
        return canBeContained;
    }

    GenTreeHWIntrinsic* hwintrinsic   = childNode->AsHWIntrinsic();
    NamedIntrinsic      intrinsicId   = hwintrinsic->GetHWIntrinsicId();
    var_types           childBaseType = hwintrinsic->GetSimdBaseType();

    bool supportsSIMDScalarLoad = supportsSIMDLoad && (expectedSize <= genTypeSize(childBaseType));

    switch (intrinsicId)
    {
        case NI_Vector128_CreateScalar:
        case NI_Vector256_CreateScalar:
        case NI_Vector512_CreateScalar:
        case NI_Vector128_CreateScalarUnsafe:
        case NI_Vector256_CreateScalarUnsafe:
        case NI_Vector512_CreateScalarUnsafe:
        {
            if (!supportsSIMDScalarLoad)
            {
                // Nothing to do if the intrinsic doesn't support scalar loads
                return false;
            }

            GenTree* op1 = hwintrinsic->Op(1);

            if (IsInvariantInRange(op1, parentNode, hwintrinsic))
            {
                if (op1->isContained() && !op1->OperIsLong())
                {
                    // We have CreateScalarUnsafe where the underlying scalar is contained
                    // As such, we can contain the CreateScalarUnsafe and consume the value
                    // directly in codegen.

                    return true;
                }

                if (op1->IsRegOptional() && varTypeIsFloating(op1))
                {
                    // We have CreateScalarUnsafe where the underlying scalar was marked reg
                    // optional. As such, we can contain the CreateScalarUnsafe and consume
                    // the value directly in codegen.
                    //
                    // We only want to do this when op1 produces a floating-point value since that means
                    // it will already be in a SIMD register in the scenario it isn't spilled.

                    return true;
                }
            }

            return false;
        }

        case NI_X86Base_LoadAlignedVector128:
        case NI_AVX_LoadAlignedVector256:
        case NI_AVX512_LoadAlignedVector512:
        {
            // In minOpts, we need to ensure that an unaligned address will fault when an explicit LoadAligned is used.
            // Non-VEX encoded instructions will fault if an unaligned SIMD16 load is contained but will not for scalar
            // loads, and VEX-encoded instructions will not fault for unaligned loads in any case.
            //
            // When optimizations are enabled, we want to contain any aligned load that is large enough for the parent's
            // requirement.

            return (supportsSIMDLoad &&
                    ((!comp->canUseVexEncoding() && expectedSize == genTypeSize(TYP_SIMD16)) || !comp->opts.MinOpts()));
        }

        case NI_X86Base_LoadScalarVector128:
        {
            // These take only pointer operands.
            assert(hwintrinsic->OperIsMemoryLoad());

            return supportsSIMDScalarLoad;
        }

        case NI_SSE42_MoveAndDuplicate:
        case NI_AVX2_BroadcastScalarToVector128:
        case NI_AVX2_BroadcastScalarToVector256:
        case NI_AVX512_BroadcastScalarToVector512:
        {
            if (comp->opts.MinOpts() || !comp->canUseEmbeddedBroadcast())
            {
                return false;
            }

            if (varTypeIsSmall(parentBaseType) || (genTypeSize(parentBaseType) != genTypeSize(childBaseType)))
            {
                // early return if either base type is not embedded broadcast compatible.
                return false;
            }

            // make the broadcast node containable when embedded broadcast can be enabled.
            if (intrinsicId == NI_SSE42_MoveAndDuplicate)
            {
                // NI_SSE42_MoveAndDuplicate is for Vector128<double> only.
                assert(childBaseType == TYP_DOUBLE);
            }

            if (parentNode->isEmbeddedBroadcastCompatibleHWIntrinsic(comp))
            {
                GenTree* broadcastOperand = hwintrinsic->Op(1);

                if (broadcastOperand->OperIsHWIntrinsic())
                {
                    GenTreeHWIntrinsic* hwintrinsicOperand = broadcastOperand->AsHWIntrinsic();
                    NamedIntrinsic      operandIntrinsicId = hwintrinsicOperand->GetHWIntrinsicId();

                    if (HWIntrinsicInfo::IsVectorCreateScalar(operandIntrinsicId) ||
                        HWIntrinsicInfo::IsVectorCreateScalarUnsafe(operandIntrinsicId))
                    {
                        // CreateScalar/Unsafe can contain non-memory operands such as enregistered
                        // locals, so we want to check if its operand is containable instead. This
                        // will result in such enregistered locals returning `false`.
                        broadcastOperand = hwintrinsicOperand->Op(1);
                    }
                }

                bool childSupportsRegOptional;
                if (IsContainableHWIntrinsicOp(hwintrinsic, broadcastOperand, &childSupportsRegOptional) &&
                    IsSafeToContainMem(parentNode, hwintrinsic))
                {
                    return true;
                }
            }
            return false;
        }

        case NI_SSE42_LoadAndDuplicateToVector128:
        case NI_AVX_BroadcastScalarToVector128:
        case NI_AVX_BroadcastScalarToVector256:
        {
            if (!comp->canUseEmbeddedBroadcast())
            {
                return false;
            }

            // These take only pointer operands.
            assert(hwintrinsic->OperIsMemoryLoad());
            assert(varTypeIsFloating(childBaseType));

            return (parentBaseType == childBaseType) && parentNode->isEmbeddedBroadcastCompatibleHWIntrinsic(comp);
        }

        default:
        {
            return false;
        }
    }
}

//----------------------------------------------------------------------------------------------
// TryFoldCnsVecForEmbeddedBroadcast:
//  Unfold the eligible constant vector when embedded broadcast is
//  available.
//
//  Arguments:
//     parentNode - The hardware intrinsic node
//     childNode  - The operand node to try contain
//
void Lowering::TryFoldCnsVecForEmbeddedBroadcast(GenTreeHWIntrinsic* parentNode, GenTreeVecCon* childNode)
{
    assert(!childNode->IsAllBitsSet());
    assert(!childNode->IsZero());

    if (!comp->canUseEmbeddedBroadcast())
    {
        MakeSrcContained(parentNode, childNode);
        return;
    }

    // We use the child node's size for the broadcast node, because the parent may consume more than its own size.
    // The containment check has already validated that the child is sufficiently large.
    //
    // We use the parent node's base type, because we must ensure that the constant repeats correctly for that size,
    // regardless of how the constant vector was created.

    var_types   simdType            = childNode->TypeGet();
    var_types   simdBaseType        = parentNode->GetSimdBaseType();
    CorInfoType simdBaseJitType     = parentNode->GetSimdBaseJitType();
    bool        isCreatedFromScalar = true;

    if (varTypeIsSmall(simdBaseType))
    {
        isCreatedFromScalar = false;
    }
    else
    {
        isCreatedFromScalar = childNode->IsBroadcast(simdBaseType);
    }

    if (isCreatedFromScalar)
    {
        NamedIntrinsic broadcastName = NI_AVX2_BroadcastScalarToVector128;
        if (simdType == TYP_SIMD32)
        {
            broadcastName = NI_AVX2_BroadcastScalarToVector256;
        }
        else if (simdType == TYP_SIMD64)
        {
            broadcastName = NI_AVX512_BroadcastScalarToVector512;
        }
        else
        {
            assert(simdType == TYP_SIMD16);
        }

        GenTree* constScalar = nullptr;
        switch (simdBaseType)
        {
            case TYP_FLOAT:
            {
                float scalar = childNode->gtSimdVal.f32[0];
                constScalar  = comp->gtNewDconNodeF(scalar);
                break;
            }
            case TYP_DOUBLE:
            {
                double scalar = childNode->gtSimdVal.f64[0];
                constScalar   = comp->gtNewDconNodeD(scalar);
                break;
            }
            case TYP_INT:
            {
                int32_t scalar = childNode->gtSimdVal.i32[0];
                constScalar    = comp->gtNewIconNode(scalar, simdBaseType);
                break;
            }
            case TYP_UINT:
            {
                uint32_t scalar = childNode->gtSimdVal.u32[0];
                constScalar     = comp->gtNewIconNode(scalar, TYP_INT);
                break;
            }
            case TYP_LONG:
            case TYP_ULONG:
            {
                int64_t scalar = childNode->gtSimdVal.i64[0];
                constScalar    = comp->gtNewLconNode(scalar);
                break;
            }
            default:
                unreached();
        }

        GenTreeHWIntrinsic* createScalar =
            comp->gtNewSimdHWIntrinsicNode(TYP_SIMD16, constScalar, NI_Vector128_CreateScalarUnsafe, simdBaseJitType,
                                           16);
        GenTreeHWIntrinsic* broadcastNode = comp->gtNewSimdHWIntrinsicNode(simdType, createScalar, broadcastName,
                                                                           simdBaseJitType, genTypeSize(simdType));
        BlockRange().InsertBefore(childNode, broadcastNode);
        BlockRange().InsertBefore(broadcastNode, createScalar);
        BlockRange().InsertBefore(createScalar, constScalar);
        LIR::Use use;
        if (BlockRange().TryGetUse(childNode, &use))
        {
            use.ReplaceWith(broadcastNode);
        }
        else
        {
            broadcastNode->SetUnusedValue();
        }

        BlockRange().Remove(childNode);
        LowerNode(createScalar);
        LowerNode(broadcastNode);
        if (varTypeIsFloating(simdBaseType))
        {
            MakeSrcContained(broadcastNode, createScalar);
        }
        else if (constScalar->TypeIs(TYP_INT, TYP_UINT, TYP_LONG, TYP_ULONG))
        {
            MakeSrcContained(broadcastNode, constScalar);
        }
        MakeSrcContained(parentNode, broadcastNode);
        return;
    }
    MakeSrcContained(parentNode, childNode);
}

//------------------------------------------------------------------------
// TryMakeSrcContainedOrRegOptional: Tries to make "childNode" a contained or regOptional node
//
//  Arguments:
//     parentNode - The hardware intrinsic node which is the parent of 'childNode'
//     childNode  - The node to check if it can be contained by 'parentNode'
//
void Lowering::TryMakeSrcContainedOrRegOptional(GenTreeHWIntrinsic* parentNode, GenTree* childNode)
{
    bool supportsRegOptional = false;

    if (IsContainableHWIntrinsicOp(parentNode, childNode, &supportsRegOptional))
    {
        if (childNode->IsCnsVec() && parentNode->isEmbeddedBroadcastCompatibleHWIntrinsic(comp))
        {
            TryFoldCnsVecForEmbeddedBroadcast(parentNode, childNode->AsVecCon());
        }
        else
        {
            MakeSrcContained(parentNode, childNode);
        }
    }
    else if (supportsRegOptional)
    {
        MakeSrcRegOptional(parentNode, childNode);
    }
}

//----------------------------------------------------------------------------------------------
// ContainCheckHWIntrinsicAddr: Perform containment analysis for an address operand of a hardware
//                              intrinsic node.
//
//  Arguments:
//     node - The hardware intrinsic node
//     addr - The address node to try contain
//     size - Size of the memory access (can be an overestimate)
//
void Lowering::ContainCheckHWIntrinsicAddr(GenTreeHWIntrinsic* node, GenTree* addr, unsigned size)
{
    assert((genActualType(addr) == TYP_I_IMPL) || addr->TypeIs(TYP_BYREF));
    if ((addr->OperIs(GT_LCL_ADDR) && IsContainableLclAddr(addr->AsLclFld(), size)) ||
        (addr->IsCnsIntOrI() && addr->AsIntConCommon()->FitsInAddrBase(comp)))
    {
        MakeSrcContained(node, addr);
    }
    else
    {
        TryCreateAddrMode(addr, true, node);
        if (addr->OperIs(GT_LEA) && IsInvariantInRange(addr, node))
        {
            MakeSrcContained(node, addr);
        }
    }
}

//----------------------------------------------------------------------------------------------
// ContainCheckHWIntrinsic: Perform containment analysis for a hardware intrinsic node.
//
//  Arguments:
//     node - The hardware intrinsic node.
//
void Lowering::ContainCheckHWIntrinsic(GenTreeHWIntrinsic* node)
{
    NamedIntrinsic      intrinsicId     = node->GetHWIntrinsicId();
    HWIntrinsicCategory category        = HWIntrinsicInfo::lookupCategory(intrinsicId);
    size_t              numArgs         = node->GetOperandCount();
    CorInfoType         simdBaseJitType = node->GetSimdBaseJitType();
    var_types           simdBaseType    = node->GetSimdBaseType();
    uint32_t            simdSize        = node->GetSimdSize();

    if (!HWIntrinsicInfo::SupportsContainment(intrinsicId))
    {
        // AVX2 gather are not containable and always have constant IMM argument
        if (HWIntrinsicInfo::isAVX2GatherIntrinsic(intrinsicId))
        {
            GenTree* lastOp = node->Op(numArgs);
            MakeSrcContained(node, lastOp);
        }
        // Exit early if containment isn't supported
        return;
    }

    bool isContainedImm = false;

    if (HWIntrinsicInfo::lookupCategory(intrinsicId) == HW_Category_IMM)
    {
        GenTree* lastOp = node->Op(numArgs);

        if (HWIntrinsicInfo::isImmOp(intrinsicId, lastOp) && lastOp->IsCnsIntOrI())
        {
            MakeSrcContained(node, lastOp);
            isContainedImm = true;
        }
    }

    if ((simdSize == 8) || (simdSize == 12))
    {
        // We want to handle GetElement/ToScalar still for Vector2/3
        if (!HWIntrinsicInfo::IsVectorToScalar(intrinsicId) && !HWIntrinsicInfo::IsVectorGetElement(intrinsicId))
        {
            // TODO-XArch-CQ: Ideally we would key this off of the size the containing node
            // expects vs the size node actually is or would be if spilled to the stack
            return;
        }
    }

    // TODO-XArch-CQ: Non-VEX encoded instructions can have both ops contained

    const bool isCommutative = node->isCommutativeHWIntrinsic();

    GenTree* op1 = nullptr;
    GenTree* op2 = nullptr;
    GenTree* op3 = nullptr;
    GenTree* op4 = nullptr;

    if (numArgs == 1)
    {
        // One argument intrinsics cannot be commutative
        assert(!isCommutative);

        op1 = node->Op(1);

        switch (category)
        {
            case HW_Category_MemoryLoad:
                ContainCheckHWIntrinsicAddr(node, op1, simdSize);
                break;

            case HW_Category_SimpleSIMD:
            case HW_Category_SIMDScalar:
            case HW_Category_Scalar:
            {
                switch (intrinsicId)
                {
                    case NI_X86Base_ReciprocalScalar:
                    case NI_X86Base_ReciprocalSqrtScalar:
                    case NI_X86Base_SqrtScalar:
                    case NI_SSE42_CeilingScalar:
                    case NI_SSE42_FloorScalar:
                    case NI_SSE42_RoundCurrentDirectionScalar:
                    case NI_SSE42_RoundToNearestIntegerScalar:
                    case NI_SSE42_RoundToNegativeInfinityScalar:
                    case NI_SSE42_RoundToPositiveInfinityScalar:
                    case NI_SSE42_RoundToZeroScalar:
                    case NI_AVX512_GetExponentScalar:
                    case NI_AVX512_Reciprocal14Scalar:
                    case NI_AVX512_ReciprocalSqrt14Scalar:
                    {
                        // These intrinsics have both 1 and 2-operand overloads.
                        //
                        // The 1-operand overload basically does `intrinsic(op1, op1)`
                        //
                        // Because of this, the operand must be loaded into a register
                        // and cannot be contained.
                        return;
                    }

                    case NI_X86Base_ConvertToInt32:
                    case NI_X86Base_X64_ConvertToInt64:
                    case NI_X86Base_ConvertToUInt32:
                    case NI_X86Base_X64_ConvertToUInt64:
                    case NI_AVX2_ConvertToInt32:
                    case NI_AVX2_ConvertToUInt32:
                    {
                        if (varTypeIsIntegral(simdBaseType))
                        {
                            // These intrinsics are "ins reg/mem, xmm" and get
                            // contained by the relevant store operation instead.
                            return;
                        }
                        break;
                    }

                    case NI_SSE42_ConvertToVector128Int16:
                    case NI_SSE42_ConvertToVector128Int32:
                    case NI_SSE42_ConvertToVector128Int64:
                    case NI_AVX2_ConvertToVector256Int16:
                    case NI_AVX2_ConvertToVector256Int32:
                    case NI_AVX2_ConvertToVector256Int64:
                    {
                        if (node->OperIsMemoryLoad())
                        {
                            ContainCheckHWIntrinsicAddr(node, op1, /* conservative maximum */ 16);
                            return;
                        }
                        break;
                    }

                    case NI_AVX2_BroadcastScalarToVector128:
                    case NI_AVX2_BroadcastScalarToVector256:
                    case NI_AVX512_BroadcastScalarToVector512:
                    {
                        if (node->OperIsMemoryLoad())
                        {
                            ContainCheckHWIntrinsicAddr(node, op1, /* conservative maximum */ 8);
                            return;
                        }

                        if (varTypeIsIntegral(simdBaseType) && op1->OperIsHWIntrinsic())
                        {
                            GenTreeHWIntrinsic* childNode      = op1->AsHWIntrinsic();
                            NamedIntrinsic      childIntrinsic = childNode->GetHWIntrinsicId();

                            if (HWIntrinsicInfo::IsVectorCreateScalar(childIntrinsic) ||
                                HWIntrinsicInfo::IsVectorCreateScalarUnsafe(childIntrinsic))
                            {
                                // We have a very special case of BroadcastScalarToVector(CreateScalar/Unsafe(op1))
                                //
                                // This is one of the only instructions where it supports taking integer types from
                                // a SIMD register or directly as a scalar from memory. Most other instructions, in
                                // comparison, take such values from general-purpose registers instead.
                                //
                                // Because of this, we're going to remove the CreateScalar/Unsafe and try to contain
                                // op1 directly, we'll then special case the codegen to materialize the value into a
                                // SIMD register in the case it is marked optional and doesn't get spilled.

                                if (childNode->Op(1)->OperIsLong())
                                {
                                    // Decomposed longs require special codegen
                                    return;
                                }

                                node->Op(1) = childNode->Op(1);
                                BlockRange().Remove(op1);

                                op1 = node->Op(1);
                                op1->ClearContained();

                                // Drop GT_CAST if it doesn't change the op1's bits we're about to broadcast
                                if (op1->OperIs(GT_CAST) && !op1->gtOverflow() && comp->opts.Tier0OptimizationEnabled())
                                {
                                    GenTreeCast* cast = op1->AsCast();
                                    if (!varTypeIsFloating(cast->CastToType()) &&
                                        !varTypeIsFloating(cast->CastFromType()) && !cast->CastOp()->OperIsLong() &&
                                        (genTypeSize(cast->CastToType()) >= genTypeSize(simdBaseType)) &&
                                        (genTypeSize(cast->CastFromType()) >= genTypeSize(simdBaseType)))
                                    {
                                        BlockRange().Remove(op1);
                                        op1 = cast->CastOp();
                                        op1->ClearContained();
                                        node->Op(1) = op1;
                                    }
                                }
                            }
                        }
                        break;
                    }

                    case NI_AVX512_ConvertToVector128UInt32:
                    case NI_AVX512_ConvertToVector128UInt32WithSaturation:
                    case NI_AVX512_ConvertToVector256Int32:
                    case NI_AVX512_ConvertToVector256UInt32:
                    {
                        if (varTypeIsFloating(simdBaseType))
                        {
                            // This version is "ins xmm, xmm/mem" and
                            // gets the default containment handling
                            break;
                        }
                        FALLTHROUGH;
                    }

                    case NI_AVX512_ConvertToVector128Byte:
                    case NI_AVX512_ConvertToVector128ByteWithSaturation:
                    case NI_AVX512_ConvertToVector128Int16:
                    case NI_AVX512_ConvertToVector128Int16WithSaturation:
                    case NI_AVX512_ConvertToVector128Int32:
                    case NI_AVX512_ConvertToVector128Int32WithSaturation:
                    case NI_AVX512_ConvertToVector128SByte:
                    case NI_AVX512_ConvertToVector128SByteWithSaturation:
                    case NI_AVX512_ConvertToVector128UInt16:
                    case NI_AVX512_ConvertToVector128UInt16WithSaturation:
                    case NI_AVX512_ConvertToVector256Byte:
                    case NI_AVX512_ConvertToVector256ByteWithSaturation:
                    case NI_AVX512_ConvertToVector256Int16:
                    case NI_AVX512_ConvertToVector256Int16WithSaturation:
                    case NI_AVX512_ConvertToVector256Int32WithSaturation:
                    case NI_AVX512_ConvertToVector256SByte:
                    case NI_AVX512_ConvertToVector256SByteWithSaturation:
                    case NI_AVX512_ConvertToVector256UInt16:
                    case NI_AVX512_ConvertToVector256UInt16WithSaturation:
                    case NI_AVX512_ConvertToVector256UInt32WithSaturation:
                    {
                        // These intrinsics are "ins reg/mem, xmm" and get
                        // contained by the relevant store operation instead.
                        return;
                    }

#ifdef TARGET_X86
                    case NI_Vector128_CreateScalar:
                    case NI_Vector256_CreateScalar:
                    case NI_Vector512_CreateScalar:
                    case NI_Vector128_CreateScalarUnsafe:
                    case NI_Vector256_CreateScalarUnsafe:
                    case NI_Vector512_CreateScalarUnsafe:
                    {
                        if (op1->OperIsLong())
                        {
                            // Contain decomposed longs and handle them in codegen
                            assert(varTypeIsLong(simdBaseType));

                            for (GenTree* longOp : op1->Operands())
                            {
                                if (!varTypeIsSmall(longOp) && IsContainableMemoryOp(longOp) &&
                                    IsSafeToContainMem(node, longOp))
                                {
                                    MakeSrcContained(node, longOp);
                                }
                                else if (IsSafeToMarkRegOptional(node, longOp))
                                {
                                    MakeSrcRegOptional(node, longOp);
                                }
                            }

                            MakeSrcContained(node, op1);
                            return;
                        }
                        break;
                    }

                    case NI_Vector128_ToScalar:
                    case NI_Vector256_ToScalar:
                    case NI_Vector512_ToScalar:
                    {
                        // These will be contained by a STOREIND
                        if (varTypeIsLong(simdBaseType))
                        {
                            return;
                        }
                        break;
                    }
#endif

                    default:
                    {
                        break;
                    }
                }

                assert(!node->OperIsMemoryLoad());

                TryMakeSrcContainedOrRegOptional(node, op1);
                break;
            }

            default:
            {
                unreached();
                break;
            }
        }
    }
    else
    {
        if (numArgs == 2)
        {
            op1 = node->Op(1);
            op2 = node->Op(2);

            switch (category)
            {
                case HW_Category_MemoryLoad:
                    if ((intrinsicId == NI_AVX_MaskLoad) || (intrinsicId == NI_AVX2_MaskLoad))
                    {
                        ContainCheckHWIntrinsicAddr(node, op1, simdSize);
                    }
                    else
                    {
                        ContainCheckHWIntrinsicAddr(node, op2, simdSize);
                    }
                    break;

                case HW_Category_MemoryStore:
                    ContainCheckHWIntrinsicAddr(node, op1, /* conservative maximum */ simdSize);
                    break;

                case HW_Category_SimpleSIMD:
                case HW_Category_SIMDScalar:
                case HW_Category_Scalar:
                {
                    bool supportsOp1RegOptional = false;
                    bool supportsOp2RegOptional = false;

                    GenTree* containedOperand   = nullptr;
                    GenTree* regOptionalOperand = nullptr;
                    bool     swapOperands       = false;

                    if (IsContainableHWIntrinsicOp(node, op2, &supportsOp2RegOptional))
                    {
                        containedOperand = op2;
                    }
                    else if (isCommutative && IsContainableHWIntrinsicOp(node, op1, &supportsOp1RegOptional))
                    {
                        containedOperand = op1;
                        swapOperands     = true;
                    }
                    else
                    {
                        if (supportsOp1RegOptional)
                        {
                            regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op1);
                        }

                        if (supportsOp2RegOptional)
                        {
                            regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op2);
                        }

                        if (regOptionalOperand == op1)
                        {
                            swapOperands = true;
                        }
                    }

                    if (containedOperand != nullptr)
                    {
                        bool isEmbeddedBroadcastCompatible =
                            containedOperand->IsCnsVec() && node->isEmbeddedBroadcastCompatibleHWIntrinsic(comp);

                        bool       isScalarArg = false;
                        genTreeOps oper        = node->GetOperForHWIntrinsicId(&isScalarArg);

                        // We want to skip trying to make this an embedded broadcast in certain scenarios
                        // because it will prevent other transforms that will be better for codegen.

                        LIR::Use use;

                        if ((oper == GT_XOR) && isEmbeddedBroadcastCompatible && BlockRange().TryGetUse(node, &use) &&
                            use.User()->OperIsVectorFusedMultiplyOp())
                        {
                            // xor is bitwise and the actual xor node might be a different base type
                            // from the FMA node, so we check if its negative zero using the FMA base
                            // type since that's what the end negation would end up using
                            var_types fmaSimdBaseType     = use.User()->AsHWIntrinsic()->GetSimdBaseType();
                            isEmbeddedBroadcastCompatible = !containedOperand->IsVectorNegativeZero(fmaSimdBaseType);
                        }

                        if (isEmbeddedBroadcastCompatible)
                        {
                            TryFoldCnsVecForEmbeddedBroadcast(node, containedOperand->AsVecCon());
                        }
                        else
                        {
                            MakeSrcContained(node, containedOperand);
                        }
                    }
                    else if (regOptionalOperand != nullptr)
                    {
                        MakeSrcRegOptional(node, regOptionalOperand);
                    }

                    if (swapOperands)
                    {
                        // Swap the operands here to make the containment checks in codegen significantly simpler
                        std::swap(node->Op(1), node->Op(2));
                    }
                    break;
                }

                case HW_Category_IMM:
                {
                    // We don't currently have any IMM intrinsics which are also commutative
                    assert(!isCommutative);

                    switch (intrinsicId)
                    {
                        case NI_X86Base_Extract:
                        case NI_AVX_ExtractVector128:
                        case NI_AVX2_ExtractVector128:
                        case NI_AVX512_ExtractVector128:
                        case NI_AVX512_ExtractVector256:
                        {
                            // These intrinsics are "ins reg/mem, xmm, imm8" and get
                            // contained by the relevant store operation instead.
                            break;
                        }

                        case NI_AVX2_Shuffle:
                        case NI_AVX512_Shuffle:
                        {
                            if (varTypeIsByte(simdBaseType))
                            {
                                // byte and sbyte are: pshufb ymm1, ymm2, ymm3/m256
                                assert(!isCommutative);

                                TryMakeSrcContainedOrRegOptional(node, op2);
                                break;
                            }
                            FALLTHROUGH;
                        }

                        case NI_X86Base_Shuffle:
                        case NI_X86Base_ShuffleHigh:
                        case NI_X86Base_ShuffleLow:
                        case NI_AVX2_Permute4x64:
                        case NI_AVX2_ShuffleHigh:
                        case NI_AVX2_ShuffleLow:
                        case NI_AVX512_ClassifyMask:
                        case NI_AVX512_ClassifyScalarMask:
                        case NI_AVX512_Permute2x64:
                        case NI_AVX512_Permute4x32:
                        case NI_AVX512_Permute4x64:
                        case NI_AVX512_ShuffleHigh:
                        case NI_AVX512_ShuffleLow:
                        case NI_AVX512_RotateLeft:
                        case NI_AVX512_RotateRight:
                        {
                            // These intrinsics have op2 as an imm and op1 as a reg/mem

                            if (!isContainedImm)
                            {
                                // Don't contain if we're generating a jmp table fallback
                                break;
                            }

                            TryMakeSrcContainedOrRegOptional(node, op1);
                            break;
                        }

                        case NI_SSE42_Extract:
                        case NI_SSE42_X64_Extract:
                        {
                            // These intrinsics are "ins reg/mem, xmm" and get
                            // contained by the relevant store operation instead.

                            assert(!varTypeIsFloating(simdBaseType));
                            break;
                        }

                        case NI_AVX_Permute:
                        case NI_X86Base_ShiftLeftLogical:
                        case NI_X86Base_ShiftRightArithmetic:
                        case NI_X86Base_ShiftRightLogical:
                        case NI_AVX2_ShiftLeftLogical:
                        case NI_AVX2_ShiftRightArithmetic:
                        case NI_AVX2_ShiftRightLogical:
                        case NI_AVX512_ShiftLeftLogical:
                        case NI_AVX512_ShiftRightArithmetic:
                        case NI_AVX512_ShiftRightLogical:
                        {
                            // These intrinsics can have op2 be imm or reg/mem
                            // They also can have op1 be reg/mem and op2 be imm

                            if (HWIntrinsicInfo::isImmOp(intrinsicId, op2))
                            {
                                if (!isContainedImm)
                                {
                                    // Don't contain if we're generating a jmp table fallback
                                    break;
                                }

                                TryMakeSrcContainedOrRegOptional(node, op1);
                            }
                            else
                            {
                                TryMakeSrcContainedOrRegOptional(node, op2);
                            }
                            break;
                        }

                        case NI_AES_KeygenAssist:
                        case NI_AVX512_GetMantissa:
                        case NI_AVX512_RoundScale:
                        case NI_AVX512_Reduce:
                        {
                            if (!isContainedImm)
                            {
                                // Don't contain if we're generating a jmp table fallback
                                break;
                            }

                            TryMakeSrcContainedOrRegOptional(node, op1);
                            break;
                        }

                        case NI_X86Base_ShiftLeftLogical128BitLane:
                        case NI_X86Base_ShiftRightLogical128BitLane:
                        case NI_AVX2_ShiftLeftLogical128BitLane:
                        case NI_AVX2_ShiftRightLogical128BitLane:
                        case NI_AVX512_ShiftLeftLogical128BitLane:
                        case NI_AVX512_ShiftRightLogical128BitLane:
                        {
                            // These intrinsics have op2 as an imm and op1 as a reg/mem when AVX512BW+VL is supported

                            if (!isContainedImm)
                            {
                                // Don't contain if we're generating a jmp table fallback
                                break;
                            }

                            TryMakeSrcContainedOrRegOptional(node, op1);
                            break;
                        }

                        case NI_AVX512_GetMantissaScalar:
                        case NI_AVX512_RoundScaleScalar:
                        case NI_AVX512_ReduceScalar:
                        {
                            // These intrinsics have both 2 and 3-operand overloads.
                            //
                            // The 2-operand overload basically does `intrinsic(op1, op1, cns)`
                            //
                            // Because of this, the operand must be loaded into a register
                            // and cannot be contained.
                            return;
                        }

                        case NI_AVX512_ShiftLeftMask:
                        case NI_AVX512_ShiftRightMask:
                        {
                            // These intrinsics don't support a memory operand and
                            // we don't currently generate a jmp table fallback.

                            assert(isContainedImm);
                            return;
                        }

                        default:
                        {
                            assert(!"Unhandled containment for binary hardware intrinsic with immediate operand");
                            break;
                        }
                    }

                    break;
                }

                case HW_Category_Helper:
                {
                    // We don't currently have any IMM intrinsics which are also commutative
                    assert(!isCommutative);

                    switch (intrinsicId)
                    {
                        case NI_Vector128_GetElement:
                        case NI_Vector256_GetElement:
                        case NI_Vector512_GetElement:
                        {
                            if (op2->OperIsConst())
                            {
                                MakeSrcContained(node, op2);
                            }

                            if (IsContainableMemoryOp(op1) && IsSafeToContainMem(node, op1))
                            {
                                MakeSrcContained(node, op1);
                            }
                            break;
                        }

                        case NI_Vector128_op_Division:
                        case NI_Vector256_op_Division:
                        {
                            break;
                        }

                        default:
                        {
                            assert(!"Unhandled containment for helper binary hardware intrinsic");
                            break;
                        }
                    }

                    break;
                }

                default:
                {
                    unreached();
                    break;
                }
            }
        }
        else if (numArgs == 3)
        {
            // three argument intrinsics should not be marked commutative
            assert(!isCommutative);

            op1 = node->Op(1);
            op2 = node->Op(2);
            op3 = node->Op(3);

            switch (category)
            {
                case HW_Category_MemoryLoad:
                {
                    if (op3->IsVectorZero())
                    {
                        // When we are merging with zero, we can specialize
                        // and avoid instantiating the vector constant.
                        MakeSrcContained(node, op3);
                    }
                    ContainCheckHWIntrinsicAddr(node, op1, simdSize);
                    break;
                }

                case HW_Category_MemoryStore:
                {
                    ContainCheckHWIntrinsicAddr(node, op1, simdSize);
                    break;
                }

                case HW_Category_SimpleSIMD:
                case HW_Category_SIMDScalar:
                case HW_Category_Scalar:
                {
                    if (HWIntrinsicInfo::IsFmaIntrinsic(intrinsicId))
                    {
                        // FMA is special in that any operand can be contained
                        // and any other operand can be the RMW operand.
                        //
                        // This comes about from having:
                        // * 132: op1 = (op1 * [op3]) + op2
                        // * 213: op2 = (op1 * op2) + [op3]
                        // * 231: op2 = (op2 * [op3]) + op1
                        //
                        // Since multiplication is commutative this gives us the
                        // full range of support to emit the best codegen.

                        bool supportsOp1RegOptional = false;
                        bool supportsOp2RegOptional = false;
                        bool supportsOp3RegOptional = false;

                        GenTree* containedOperand   = nullptr;
                        GenTree* regOptionalOperand = nullptr;

                        LIR::Use use;
                        GenTree* user = nullptr;

                        if (BlockRange().TryGetUse(node, &use))
                        {
                            user = use.User();
                        }
                        unsigned resultOpNum = node->GetResultOpNumForRmwIntrinsic(user, op1, op2, op3);

                        // Prioritize Containable op. Check if any one of the op is containable first.
                        // Set op regOptional only if none of them is containable.

                        // Prefer to make op3 contained as it doesn't require reordering operands
                        if ((resultOpNum != 3) && IsContainableHWIntrinsicOp(node, op3, &supportsOp3RegOptional))
                        {
                            // result = (op1 * op2) + [op3]
                            containedOperand = op3;
                        }
                        else if ((resultOpNum != 2) && IsContainableHWIntrinsicOp(node, op2, &supportsOp2RegOptional))
                        {
                            // result = (op1 * [op2]) + op3
                            containedOperand = op2;
                        }
                        else if ((resultOpNum != 1) && !HWIntrinsicInfo::CopiesUpperBits(intrinsicId) &&
                                 IsContainableHWIntrinsicOp(node, op1, &supportsOp1RegOptional))
                        {
                            // result = ([op1] * op2) + op3
                            containedOperand = op1;
                        }
                        else
                        {
                            if (supportsOp1RegOptional)
                            {
                                regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op1);
                            }

                            if (supportsOp2RegOptional)
                            {
                                regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op2);
                            }

                            if (supportsOp3RegOptional)
                            {
                                regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op3);
                            }
                        }

                        if (containedOperand != nullptr)
                        {
                            if (containedOperand->IsCnsVec() && node->isEmbeddedBroadcastCompatibleHWIntrinsic(comp))
                            {
                                TryFoldCnsVecForEmbeddedBroadcast(node, containedOperand->AsVecCon());
                            }
                            else
                            {
                                MakeSrcContained(node, containedOperand);
                            }
                        }
                        else if (regOptionalOperand != nullptr)
                        {
                            MakeSrcRegOptional(node, regOptionalOperand);
                        }
                    }
                    else if (HWIntrinsicInfo::IsPermuteVar2x(intrinsicId))
                    {
                        assert(comp->canUseEvexEncodingDebugOnly());

                        // PermuteVar2x is similarly special in that op1 and op3
                        // are commutative and op1 or op2 can be the RMW operand.
                        //
                        // This comes about from having:
                        // * i2: op2 = permutex2var(op1, op2, op3)
                        // * t2: op1 = permutex2var(op1, op2, op3)
                        //
                        // Given op1 and op3 are commutative this also gives us the full
                        // range of support. However, given we can only swap op1/op3 if
                        // we toggle a bit in the indices (op2) and the cost of this is
                        // another memory load if op2 isn't constant, we don't swap in that
                        // case to avoid another memory access for the toggle operand

                        bool supportsOp1RegOptional = false;
                        bool supportsOp3RegOptional = false;

                        GenTree* containedOperand   = nullptr;
                        GenTree* regOptionalOperand = nullptr;
                        bool     swapOperands       = false;
                        bool     isOp2Cns           = op2->IsCnsVec();

                        LIR::Use use;
                        GenTree* user = nullptr;

                        if (BlockRange().TryGetUse(node, &use))
                        {
                            user = use.User();
                        }
                        unsigned resultOpNum = node->GetResultOpNumForRmwIntrinsic(user, op1, op2, op3);

                        // Prioritize Containable op. Check if any one of the op is containable first.
                        // Set op regOptional only if none of them is containable.

                        // Prefer to make op3 contained as it doesn't require reordering operands
                        if (((resultOpNum != 3) || !isOp2Cns) &&
                            IsContainableHWIntrinsicOp(node, op3, &supportsOp3RegOptional))
                        {
                            containedOperand = op3;
                        }
                        else if ((resultOpNum != 2) && isOp2Cns &&
                                 IsContainableHWIntrinsicOp(node, op1, &supportsOp1RegOptional))
                        {
                            containedOperand = op1;
                            swapOperands     = true;
                        }
                        else
                        {
                            if (supportsOp1RegOptional)
                            {
                                regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op1);
                            }

                            if (supportsOp3RegOptional)
                            {
                                regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op3);
                            }

                            if (regOptionalOperand == op1)
                            {
                                swapOperands = true;
                            }
                        }

                        if (containedOperand != nullptr)
                        {
                            if (containedOperand->IsCnsVec() && node->isEmbeddedBroadcastCompatibleHWIntrinsic(comp))
                            {
                                TryFoldCnsVecForEmbeddedBroadcast(node, containedOperand->AsVecCon());
                            }
                            else
                            {
                                MakeSrcContained(node, containedOperand);
                            }
                        }
                        else if (regOptionalOperand != nullptr)
                        {
                            MakeSrcRegOptional(node, regOptionalOperand);
                        }

                        if (swapOperands)
                        {
                            // Swap the operands here to make the containment checks in codegen significantly simpler
                            assert(op2->IsCnsVec());
                            std::swap(node->Op(1), node->Op(3));

                            uint32_t elemSize  = genTypeSize(simdBaseType);
                            uint32_t elemCount = simdSize / elemSize;
                            uint64_t toggleBit = 0;

                            switch (elemSize)
                            {
                                case 1:
                                {
                                    // We pick a base uint8_t of:
                                    // * TYP_SIMD16: 0x10
                                    // * TYP_SIMD32: 0x20
                                    // * TYP_SIMD64: 0x40
                                    switch (simdSize)
                                    {
                                        case 16:
                                            toggleBit = 0x1010101010101010;
                                            break;
                                        case 32:
                                            toggleBit = 0x2020202020202020;
                                            break;
                                        default:
                                            assert(simdSize == 64);
                                            toggleBit = 0x4040404040404040;
                                            break;
                                    }
                                    break;
                                }

                                case 2:
                                {
                                    // We pick a base uint16_t of:
                                    // * TYP_SIMD16: 0x08
                                    // * TYP_SIMD32: 0x10
                                    // * TYP_SIMD64: 0x20
                                    switch (simdSize)
                                    {
                                        case 16:
                                            toggleBit = 0x0008000800080008;
                                            break;
                                        case 32:
                                            toggleBit = 0x0010001000100010;
                                            break;
                                        default:
                                            assert(simdSize == 64);
                                            toggleBit = 0x0020002000200020;
                                            break;
                                    }
                                    break;
                                }

                                case 4:
                                {
                                    // We pick a base uint32_t of:
                                    // * TYP_SIMD16: 0x04
                                    // * TYP_SIMD32: 0x08
                                    // * TYP_SIMD64: 0x10
                                    switch (simdSize)
                                    {
                                        case 16:
                                            toggleBit = 0x0000000400000004;
                                            break;
                                        case 32:
                                            toggleBit = 0x0000000800000008;
                                            break;
                                        default:
                                            assert(simdSize == 64);
                                            toggleBit = 0x0000001000000010;
                                            break;
                                    }
                                    break;
                                }

                                case 8:
                                {
                                    // We pick a base uint32_t of:
                                    // * TYP_SIMD16: 0x02
                                    // * TYP_SIMD32: 0x04
                                    // * TYP_SIMD64: 0x08
                                    switch (simdSize)
                                    {
                                        case 16:
                                            toggleBit = 0x0000000000000002;
                                            break;
                                        case 32:
                                            toggleBit = 0x0000000000000004;
                                            break;
                                        default:
                                            assert(simdSize == 64);
                                            toggleBit = 0x0000000000000008;
                                            break;
                                    }
                                    break;
                                }

                                default:
                                {
                                    unreached();
                                }
                            }

                            GenTreeVecCon* vecCon = op2->AsVecCon();

                            for (uint32_t i = 0; i < (simdSize / 8); i++)
                            {
                                vecCon->gtSimdVal.u64[i] ^= toggleBit;
                            }
                        }
                    }
                    else
                    {
                        switch (intrinsicId)
                        {
                            case NI_SSE42_BlendVariable:
                            case NI_AVX_BlendVariable:
                            case NI_AVX2_BlendVariable:
                            {
                                TryMakeSrcContainedOrRegOptional(node, op2);
                                break;
                            }

                            case NI_AVX512_BlendVariableMask:
                            {
                                // BlendVariableMask represents one of the following instructions:
                                // * vblendmpd
                                // * vblendmps
                                // * vpblendmpb
                                // * vpblendmpd
                                // * vpblendmpq
                                // * vpblendmpw
                                //
                                // In all cases, the node operands are ordered:
                                // * op1: selectFalse
                                // * op2: selectTrue
                                // * op3: condition
                                //
                                // The managed API surface we expose doesn't directly support TYP_MASK
                                // and we don't directly expose overloads for APIs like `vaddps` which
                                // support embedded masking. Instead, we have decide to do pattern
                                // recognition over the relevant ternary select APIs which functionally
                                // execute `cond ? selectTrue : selectFalse` on a per element basis.
                                //
                                // To facilitate this, the mentioned ternary select APIs, such as
                                // ConditionalSelect or TernaryLogic, with a correct control word, will
                                // all compile down to BlendVariableMask when the condition is of TYP_MASK.
                                //
                                // So, before we do the normal containment checks for memory operands, we
                                // instead want to check if `selectTrue` (op2) supports embedded masking and
                                // if so, we want to mark it as contained. Codegen will then see that it is
                                // contained and not a memory operand and know to invoke the special handling
                                // so that the embedded masking can work as expected.

                                if (op1->IsVectorZero())
                                {
                                    // When we are merging with zero, we can specialize
                                    // and avoid instantiating the vector constant.
                                    MakeSrcContained(node, op1);
                                }

                                if (op2->isEmbeddedMaskingCompatibleHWIntrinsic())
                                {
                                    bool isEmbeddedMask = !comp->opts.MinOpts() && comp->canUseEmbeddedMasking();

                                    if (op2->isRMWHWIntrinsic(comp))
                                    {
                                        // TODO-AVX512-CQ: Ensure we can support embedded operations on RMW intrinsics
                                        isEmbeddedMask = false;
                                    }

                                    GenTreeHWIntrinsic* op2Intrinsic   = op2->AsHWIntrinsic();
                                    NamedIntrinsic      op2IntrinsicId = NI_Illegal;
                                    HWIntrinsicCategory category       = HW_Category_Special;

                                    if (isEmbeddedMask)
                                    {
                                        // TODO-AVX512-CQ: Codegen is currently limited to only handling embedded
                                        // masking for table driven intrinsics. This can be relaxed once that is fixed.

                                        op2IntrinsicId = op2Intrinsic->GetHWIntrinsicId();
                                        category       = HWIntrinsicInfo::lookupCategory(op2IntrinsicId);
                                        isEmbeddedMask =
                                            HWIntrinsicInfo::genIsTableDrivenHWIntrinsic(op2IntrinsicId, category);

                                        size_t numArgs = node->GetOperandCount();

                                        if (numArgs == 1)
                                        {
                                            if (op2Intrinsic->OperIsMemoryLoad())
                                            {
                                                isEmbeddedMask = false;
                                            }
                                        }
                                        else if (numArgs == 2)
                                        {
                                            if (category == HW_Category_MemoryStore)
                                            {
                                                isEmbeddedMask = false;
                                            }
                                        }
                                    }

                                    if (isEmbeddedMask)
                                    {
                                        var_types op2SimdBaseType = op2Intrinsic->GetSimdBaseType();

                                        instruction ins =
                                            HWIntrinsicInfo::lookupIns(op2IntrinsicId, op2SimdBaseType, comp);

                                        unsigned expectedMaskBaseSize = CodeGenInterface::instKMaskBaseSize(ins);

                                        // It's safe to use the return and base type of the BlendVariableMask node
                                        // since anything which lowered to it will have validated compatibility itself
                                        unsigned actualMaskSize =
                                            genTypeSize(node->TypeGet()) / genTypeSize(simdBaseType);
                                        unsigned actualMaskBaseSize =
                                            actualMaskSize / (genTypeSize(node->TypeGet()) / 16);

                                        CorInfoType op2AdjustedSimdBaseJitType = CORINFO_TYPE_UNDEF;

                                        if (actualMaskBaseSize != expectedMaskBaseSize)
                                        {
                                            // Some intrinsics are effectively bitwise operations and so we
                                            // can freely update them to match the size of the actual mask

                                            bool supportsMaskBaseSize4Or8 = false;

                                            switch (ins)
                                            {
                                                case INS_andpd:
                                                case INS_andps:
                                                case INS_andnpd:
                                                case INS_andnps:
                                                case INS_orpd:
                                                case INS_orps:
                                                case INS_pandd:
                                                case INS_pandnd:
                                                case INS_pord:
                                                case INS_pxord:
                                                case INS_vpandq:
                                                case INS_vpandnq:
                                                case INS_vporq:
                                                case INS_vpxorq:
                                                case INS_vshuff32x4:
                                                case INS_vshuff64x2:
                                                case INS_vshufi32x4:
                                                case INS_vshufi64x2:
                                                case INS_xorpd:
                                                case INS_xorps:
                                                {
                                                    // These intrinsics support embedded broadcast and have masking
                                                    // support for 4 or 8
                                                    assert((expectedMaskBaseSize == 4) || (expectedMaskBaseSize == 8));

                                                    if (!comp->codeGen->IsEmbeddedBroadcastEnabled(ins,
                                                                                                   op2Intrinsic->Op(2)))
                                                    {
                                                        // We cannot change the base type if we've already contained a
                                                        // broadcast
                                                        supportsMaskBaseSize4Or8 = true;
                                                    }
                                                    break;
                                                }

                                                case INS_vpternlogd:
                                                case INS_vpternlogq:
                                                {
                                                    // These intrinsics support embedded broadcast and have masking
                                                    // support for 4 or 8
                                                    assert((expectedMaskBaseSize == 4) || (expectedMaskBaseSize == 8));

                                                    if (!comp->codeGen->IsEmbeddedBroadcastEnabled(ins,
                                                                                                   op2Intrinsic->Op(3)))
                                                    {
                                                        // We cannot change the base type if we've already contained a
                                                        // broadcast
                                                        supportsMaskBaseSize4Or8 = true;
                                                    }
                                                    break;
                                                }

                                                case INS_vbroadcastf32x4:
                                                case INS_vbroadcastf32x8:
                                                case INS_vbroadcastf64x2:
                                                case INS_vbroadcastf64x4:
                                                case INS_vbroadcasti32x4:
                                                case INS_vbroadcasti32x8:
                                                case INS_vbroadcasti64x2:
                                                case INS_vbroadcasti64x4:
                                                case INS_vextractf32x4:
                                                case INS_vextractf32x8:
                                                case INS_vextractf64x2:
                                                case INS_vextractf64x4:
                                                case INS_vextracti32x4:
                                                case INS_vextracti32x8:
                                                case INS_vextracti64x2:
                                                case INS_vextracti64x4:
                                                case INS_vinsertf32x4:
                                                case INS_vinsertf32x8:
                                                case INS_vinsertf64x2:
                                                case INS_vinsertf64x4:
                                                case INS_vinserti32x4:
                                                case INS_vinserti32x8:
                                                case INS_vinserti64x2:
                                                case INS_vinserti64x4:
                                                {
                                                    // These intrinsics don't support embedded broadcast and have
                                                    // masking support for 4 or 8
                                                    assert((expectedMaskBaseSize == 4) || (expectedMaskBaseSize == 8));
                                                    supportsMaskBaseSize4Or8 = true;
                                                    break;
                                                }

                                                default:
                                                {
                                                    break;
                                                }
                                            }

                                            if (supportsMaskBaseSize4Or8)
                                            {
                                                if (actualMaskBaseSize == 8)
                                                {
                                                    if (varTypeIsFloating(op2SimdBaseType))
                                                    {
                                                        op2AdjustedSimdBaseJitType = CORINFO_TYPE_DOUBLE;
                                                    }
                                                    else if (varTypeIsSigned(op2SimdBaseType))
                                                    {
                                                        op2AdjustedSimdBaseJitType = CORINFO_TYPE_LONG;
                                                    }
                                                    else
                                                    {
                                                        op2AdjustedSimdBaseJitType = CORINFO_TYPE_ULONG;
                                                    }
                                                }
                                                else if (actualMaskBaseSize == 4)
                                                {
                                                    if (varTypeIsFloating(op2SimdBaseType))
                                                    {
                                                        op2AdjustedSimdBaseJitType = CORINFO_TYPE_FLOAT;
                                                    }
                                                    else if (varTypeIsSigned(op2SimdBaseType))
                                                    {
                                                        op2AdjustedSimdBaseJitType = CORINFO_TYPE_INT;
                                                    }
                                                    else
                                                    {
                                                        op2AdjustedSimdBaseJitType = CORINFO_TYPE_UINT;
                                                    }
                                                }
                                            }
                                        }

                                        if (op2AdjustedSimdBaseJitType != CORINFO_TYPE_UNDEF)
                                        {
                                            ins = HWIntrinsicInfo::lookupIns(op2IntrinsicId, op2SimdBaseType, comp);
                                            expectedMaskBaseSize = CodeGenInterface::instKMaskBaseSize(ins);
                                        }

                                        unsigned expectedMaskSize =
                                            expectedMaskBaseSize * (genTypeSize(op2->TypeGet()) / 16);
                                        assert(expectedMaskSize != 0);

                                        if (actualMaskSize != expectedMaskSize)
                                        {
                                            isEmbeddedMask = false;
                                        }
                                        else if (op2AdjustedSimdBaseJitType != CORINFO_TYPE_UNDEF)
                                        {
                                            op2Intrinsic->SetSimdBaseJitType(op2AdjustedSimdBaseJitType);
                                        }
                                    }

                                    if (isEmbeddedMask && IsInvariantInRange(op2, node))
                                    {
                                        MakeSrcContained(node, op2);
                                        op2->MakeEmbMaskOp();
                                        break;
                                    }
                                }

                                TryMakeSrcContainedOrRegOptional(node, op2);
                                break;
                            }

                            case NI_AVX512_CompressMask:
                            case NI_AVX512_ExpandMask:
                            {
                                if (op1->IsVectorZero())
                                {
                                    // When we are merging with zero, we can specialize
                                    // and avoid instantiating the vector constant.
                                    MakeSrcContained(node, op1);
                                }
                                break;
                            }

                            case NI_AVX2_MultiplyNoFlags:
                            case NI_AVX2_X64_MultiplyNoFlags:
                            {
                                bool supportsOp1RegOptional = false;
                                bool supportsOp2RegOptional = false;

                                GenTree* containedOperand   = nullptr;
                                GenTree* regOptionalOperand = nullptr;
                                bool     swapOperands       = false;

                                if (IsContainableHWIntrinsicOp(node, op2, &supportsOp2RegOptional))
                                {
                                    containedOperand = op2;
                                }
                                else if (IsContainableHWIntrinsicOp(node, op1, &supportsOp1RegOptional))
                                {
                                    containedOperand = op1;
                                    swapOperands     = true;
                                }
                                else
                                {
                                    if (supportsOp1RegOptional)
                                    {
                                        regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op1);
                                    }

                                    if (supportsOp2RegOptional)
                                    {
                                        regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op2);
                                    }

                                    if (regOptionalOperand == op1)
                                    {
                                        swapOperands = true;
                                    }
                                }

                                if (containedOperand != nullptr)
                                {
                                    MakeSrcContained(node, containedOperand);
                                }
                                else if (regOptionalOperand != nullptr)
                                {
                                    MakeSrcRegOptional(node, regOptionalOperand);
                                }

                                if (swapOperands)
                                {
                                    // Swap the operands here to make the containment checks in codegen significantly
                                    // simpler
                                    std::swap(node->Op(1), node->Op(2));
                                }
                                break;
                            }

                            default:
                            {
                                assert((intrinsicId == NI_X86Base_DivRem) || (intrinsicId == NI_X86Base_X64_DivRem) ||
                                       (intrinsicId >= FIRST_NI_AVXVNNI && intrinsicId <= LAST_NI_AVXVNNIINT_V512));
                                TryMakeSrcContainedOrRegOptional(node, op3);
                                break;
                            }
                        }
                    }
                    break;
                }

                case HW_Category_IMM:
                {
                    switch (intrinsicId)
                    {
                        case NI_X86Base_Shuffle:
                        case NI_X86Base_Insert:
                        case NI_SSE42_AlignRight:
                        case NI_SSE42_Blend:
                        case NI_SSE42_DotProduct:
                        case NI_SSE42_X64_Insert:
                        case NI_SSE42_MultipleSumAbsoluteDifferences:
                        case NI_AVX_Blend:
                        case NI_AVX_Compare:
                        case NI_AVX_CompareScalar:
                        case NI_AVX_DotProduct:
                        case NI_AVX_InsertVector128:
                        case NI_AVX_Permute2x128:
                        case NI_AVX_Shuffle:
                        case NI_AVX2_AlignRight:
                        case NI_AVX2_Blend:
                        case NI_AVX2_InsertVector128:
                        case NI_AVX2_MultipleSumAbsoluteDifferences:
                        case NI_AVX2_Permute2x128:
                        case NI_AVX512_AlignRight32:
                        case NI_AVX512_AlignRight64:
                        case NI_AVX512_AlignRight:
                        case NI_AVX512_GetMantissaScalar:
                        case NI_AVX512_InsertVector128:
                        case NI_AVX512_InsertVector256:
                        case NI_AVX512_Range:
                        case NI_AVX512_RangeScalar:
                        case NI_AVX512_ReduceScalar:
                        case NI_AVX512_RoundScaleScalar:
                        case NI_AVX512_Shuffle2x128:
                        case NI_AVX512_Shuffle4x128:
                        case NI_AVX512_Shuffle:
                        case NI_AVX512_SumAbsoluteDifferencesInBlock32:
                        case NI_AVX512_CompareMask:
                        case NI_AES_CarrylessMultiply:
                        case NI_AES_V256_CarrylessMultiply:
                        case NI_AES_V512_CarrylessMultiply:
                        case NI_AVX10v2_MinMax:
                        case NI_AVX10v2_MinMaxScalar:
                        case NI_AVX10v2_MultipleSumAbsoluteDifferences:
                        case NI_GFNI_GaloisFieldAffineTransform:
                        case NI_GFNI_GaloisFieldAffineTransformInverse:
                        case NI_GFNI_V256_GaloisFieldAffineTransform:
                        case NI_GFNI_V256_GaloisFieldAffineTransformInverse:
                        case NI_GFNI_V512_GaloisFieldAffineTransform:
                        case NI_GFNI_V512_GaloisFieldAffineTransformInverse:
                        {
                            if (!isContainedImm)
                            {
                                // Don't contain if we're generating a jmp table fallback
                                break;
                            }

                            TryMakeSrcContainedOrRegOptional(node, op2);
                            break;
                        }

                        case NI_SSE42_Insert:
                        {
                            GenTree* lastOp = node->Op(numArgs);

                            if (!isContainedImm)
                            {
                                // Don't contain if we're generating a jmp table fallback
                                break;
                            }

                            if (simdBaseType == TYP_FLOAT)
                            {
                                assert(lastOp->IsCnsIntOrI());

                                // Sse41.Insert has:
                                //  * Bits 0-3: zmask
                                //  * Bits 4-5: count_d
                                //  * Bits 6-7: count_s (register form only)
                                //
                                // Where zmask specifies which elements to zero
                                // Where count_d specifies the destination index the value is being inserted to
                                // Where count_s specifies the source index of the value being inserted

                                if (op1->IsVectorZero())
                                {
                                    // When op1 is zero, we can contain it and we expect that
                                    // ival is already in the correct state to account for it

#if DEBUG
                                    ssize_t ival = lastOp->AsIntConCommon()->IconValue();

                                    ssize_t zmask   = (ival & 0x0F);
                                    ssize_t count_d = (ival & 0x30) >> 4;
                                    ssize_t count_s = (ival & 0xC0) >> 6;

                                    zmask |= ~(ssize_t(1) << count_d);
                                    zmask &= 0x0F;

                                    ssize_t expected = (count_s << 6) | (count_d << 4) | (zmask);
                                    assert(ival == expected);
#endif

                                    MakeSrcContained(node, op1);
                                }
                                else if (op2->IsVectorZero())
                                {
                                    // When op2 is zero, we can contain it and we expect that
                                    // zmask is already in the correct state to account for it

#if DEBUG
                                    ssize_t ival = lastOp->AsIntConCommon()->IconValue();

                                    ssize_t zmask   = (ival & 0x0F);
                                    ssize_t count_d = (ival & 0x30) >> 4;
                                    ssize_t count_s = (ival & 0xC0) >> 6;

                                    zmask |= (ssize_t(1) << count_d);
                                    zmask &= 0x0F;

                                    ssize_t expected = (count_s << 6) | (count_d << 4) | (zmask);
                                    assert(ival == expected);
#endif

                                    MakeSrcContained(node, op2);
                                }
                            }

                            TryMakeSrcContainedOrRegOptional(node, op2);
                            break;
                        }

                        default:
                        {
                            assert(!"Unhandled containment for ternary hardware intrinsic with immediate operand");
                            break;
                        }
                    }

                    break;
                }

                default:
                {
                    unreached();
                    break;
                }
            }
        }
        else if (numArgs == 4)
        {
            // four argument intrinsics should not be marked commutative
            assert(!isCommutative);

            op1 = node->Op(1);
            op2 = node->Op(2);
            op3 = node->Op(3);
            op4 = node->Op(4);

            switch (category)
            {
                case HW_Category_IMM:
                {
                    switch (intrinsicId)
                    {
                        case NI_AVX512_Fixup:
                        case NI_AVX512_FixupScalar:
                        {
                            if (!isContainedImm)
                            {
                                // Don't contain if we're generating a jmp table fallback
                                break;
                            }

                            TryMakeSrcContainedOrRegOptional(node, op3);

                            if (!node->isRMWHWIntrinsic(comp))
                            {
                                // op1 is never selected by the table so
                                // we should've replaced it with a containable
                                // constant, allowing us to get better non-RMW
                                // codegen

                                assert(op1->IsCnsVec());
                                MakeSrcContained(node, op1);
                            }
                            break;
                        }

                        case NI_AVX512_TernaryLogic:
                        {
                            assert(comp->canUseEvexEncodingDebugOnly());

                            // These are the control bytes used for TernaryLogic

                            const uint8_t A = 0xF0;
                            const uint8_t B = 0xCC;
                            const uint8_t C = 0xAA;

                            if (!isContainedImm)
                            {
                                // Don't contain if we're generating a jmp table fallback
                                break;
                            }

                            uint8_t                 control  = static_cast<uint8_t>(op4->AsIntCon()->gtIconVal);
                            const TernaryLogicInfo* info     = &TernaryLogicInfo::lookup(control);
                            TernaryLogicUseFlags    useFlags = info->GetAllUseFlags();

                            bool supportsOp1RegOptional = false;
                            bool supportsOp2RegOptional = false;
                            bool supportsOp3RegOptional = false;

                            GenTree*             containedOperand   = nullptr;
                            GenTree*             regOptionalOperand = nullptr;
                            TernaryLogicUseFlags swapOperands       = TernaryLogicUseFlags::None;

                            switch (useFlags)
                            {
                                case TernaryLogicUseFlags::None:
                                {
                                    break;
                                }

                                case TernaryLogicUseFlags::C:
                                {
                                    // We're only using op3, so that's the one to try and contain

                                    assert(op1->IsCnsVec());
                                    MakeSrcContained(node, op1);

                                    assert(op2->IsCnsVec());
                                    MakeSrcContained(node, op2);

                                    if (IsContainableHWIntrinsicOp(node, op3, &supportsOp3RegOptional))
                                    {
                                        containedOperand = op3;
                                    }
                                    else if (supportsOp3RegOptional)
                                    {
                                        regOptionalOperand = op3;
                                    }
                                    break;
                                }

                                case TernaryLogicUseFlags::BC:
                                {
                                    // We're only using op2 and op3, so find the right one to contain
                                    // using the standard commutative rules, fixing up the control byte
                                    // as needed to ensure the operation remains the same

                                    assert(op1->IsCnsVec());
                                    MakeSrcContained(node, op1);

                                    if (IsContainableHWIntrinsicOp(node, op3, &supportsOp3RegOptional))
                                    {
                                        containedOperand = op3;
                                    }
                                    else if (IsContainableHWIntrinsicOp(node, op2, &supportsOp2RegOptional))
                                    {
                                        containedOperand = op2;
                                        swapOperands     = TernaryLogicUseFlags::BC;
                                    }
                                    else
                                    {
                                        if (supportsOp2RegOptional)
                                        {
                                            regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op2);
                                        }

                                        if (supportsOp3RegOptional)
                                        {
                                            regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op3);
                                        }

                                        if (regOptionalOperand == op2)
                                        {
                                            swapOperands = TernaryLogicUseFlags::BC;
                                        }
                                    }
                                    break;
                                }

                                case TernaryLogicUseFlags::ABC:
                                {
                                    // TernaryLogic is special in that any operand can be contained
                                    // and any other operand can be the RMW operand.
                                    //
                                    // This comes about from having a control byte that indicates
                                    // the operation to be performed per operand.

                                    LIR::Use use;
                                    GenTree* user = nullptr;

                                    if (BlockRange().TryGetUse(node, &use))
                                    {
                                        user = use.User();
                                    }
                                    unsigned resultOpNum = node->GetResultOpNumForRmwIntrinsic(user, op1, op2, op3);

                                    // Prioritize Containable op. Check if any one of the op is containable first.
                                    // Set op regOptional only if none of them is containable.

                                    if (resultOpNum == 2)
                                    {
                                        // Swap the operands here to make the containment checks in codegen
                                        // significantly simpler
                                        std::swap(node->Op(1), node->Op(2));
                                        std::swap(op1, op2);

                                        // Make sure we also fixup the control byte
                                        control = TernaryLogicInfo::GetTernaryControlByte(*info, B, A, C);
                                        op4->AsIntCon()->SetIconValue(control);

                                        // Result is now in op1, but also get the updated info
                                        resultOpNum = 1;
                                        info        = &TernaryLogicInfo::lookup(control);
                                    }
                                    else if (resultOpNum == 3)
                                    {
                                        // Swap the operands here to make the containment checks in codegen
                                        // significantly simpler
                                        std::swap(node->Op(1), node->Op(3));
                                        std::swap(op1, op3);

                                        // Make sure we also fixup the control byte
                                        control = TernaryLogicInfo::GetTernaryControlByte(*info, C, B, A);
                                        op4->AsIntCon()->SetIconValue(control);

                                        // Result is now in op1, but also get the updated info
                                        resultOpNum = 1;
                                        info        = &TernaryLogicInfo::lookup(control);
                                    }

                                    // Prefer to make op3 contained as it doesn't require reordering operands
                                    if (IsContainableHWIntrinsicOp(node, op3, &supportsOp3RegOptional))
                                    {
                                        containedOperand = op3;
                                    }
                                    else if (IsContainableHWIntrinsicOp(node, op2, &supportsOp2RegOptional))
                                    {
                                        containedOperand = op2;
                                        swapOperands     = TernaryLogicUseFlags::BC;
                                    }
                                    else if ((resultOpNum != 1) &&
                                             IsContainableHWIntrinsicOp(node, op1, &supportsOp1RegOptional))
                                    {
                                        containedOperand = op1;
                                        swapOperands     = TernaryLogicUseFlags::AC;
                                    }
                                    else
                                    {
                                        if (supportsOp1RegOptional)
                                        {
                                            regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op1);
                                        }

                                        if (supportsOp2RegOptional)
                                        {
                                            regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op2);
                                        }

                                        if (supportsOp3RegOptional)
                                        {
                                            regOptionalOperand = PreferredRegOptionalOperand(regOptionalOperand, op3);
                                        }

                                        if (regOptionalOperand == op1)
                                        {
                                            swapOperands = TernaryLogicUseFlags::AC;
                                        }
                                        else if (regOptionalOperand == op2)
                                        {
                                            swapOperands = TernaryLogicUseFlags::BC;
                                        }
                                    }
                                    break;
                                }

                                default:
                                {
                                    // Lowering should have normalized to one of the above
                                    unreached();
                                }
                            }

                            if (containedOperand != nullptr)
                            {
                                if (containedOperand->IsCnsVec() &&
                                    node->isEmbeddedBroadcastCompatibleHWIntrinsic(comp))
                                {
                                    TryFoldCnsVecForEmbeddedBroadcast(node, containedOperand->AsVecCon());
                                }
                                else
                                {
                                    MakeSrcContained(node, containedOperand);
                                }
                            }
                            else if (regOptionalOperand != nullptr)
                            {
                                MakeSrcRegOptional(node, regOptionalOperand);
                            }

                            if (swapOperands == TernaryLogicUseFlags::AC)
                            {
                                // Swap the operands here to make the containment checks in codegen
                                // significantly simpler
                                std::swap(node->Op(1), node->Op(3));

                                // Make sure we also fixup the control byte
                                control = TernaryLogicInfo::GetTernaryControlByte(*info, C, B, A);
                                op4->AsIntCon()->SetIconValue(control);
                            }
                            else if (swapOperands == TernaryLogicUseFlags::BC)
                            {
                                // Swap the operands here to make the containment checks in codegen
                                // significantly simpler
                                std::swap(node->Op(2), node->Op(3));

                                // Make sure we also fixup the control byte
                                control = TernaryLogicInfo::GetTernaryControlByte(*info, A, C, B);
                                op4->AsIntCon()->SetIconValue(control);
                            }
                            else
                            {
                                assert(swapOperands == TernaryLogicUseFlags::None);
                            }
                            break;
                        }

                        default:
                        {
                            assert(!"Unhandled containment for quaternary hardware intrinsic with immediate operand");
                            break;
                        }
                    }
                    break;
                }

                default:
                {
                    unreached();
                    break;
                }
            }
        }
        else
        {
            unreached();
        }
    }
}
#endif // FEATURE_HW_INTRINSICS

//------------------------------------------------------------------------
// ContainCheckFloatBinary: determine whether the sources of a floating point binary node should be contained.
//
// Arguments:
//    node - pointer to the node
//
void Lowering::ContainCheckFloatBinary(GenTreeOp* node)
{
    assert(node->OperIs(GT_ADD, GT_SUB, GT_MUL, GT_DIV) && varTypeIsFloating(node));

    // overflow operations aren't supported on float/double types.
    assert(!node->gtOverflowEx());

    GenTree* op1 = node->gtGetOp1();
    GenTree* op2 = node->gtGetOp2();

    // No implicit conversions at this stage as the expectation is that
    // everything is made explicit by adding casts.
    assert(op1->TypeGet() == op2->TypeGet());

    bool isSafeToContainOp1 = true;
    bool isSafeToContainOp2 = true;

    if (op2->IsCnsNonZeroFltOrDbl())
    {
        MakeSrcContained(node, op2);
    }
    else if (IsContainableMemoryOp(op2))
    {
        isSafeToContainOp2 = IsSafeToContainMem(node, op2);
        if (isSafeToContainOp2)
        {
            MakeSrcContained(node, op2);
        }
    }

    if (!op2->isContained() && node->OperIsCommutative())
    {
        // Though we have GT_ADD(op1=memOp, op2=non-memOp, we try to reorder the operands
        // as long as it is safe so that the following efficient code sequence is generated:
        //      addss/sd targetReg, memOp    (if op1Reg == targetReg) OR
        //      movaps targetReg, op2Reg; addss/sd targetReg, [memOp]
        //
        // Instead of
        //      movss op1Reg, [memOp]; addss/sd targetReg, Op2Reg  (if op1Reg == targetReg) OR
        //      movss op1Reg, [memOp]; movaps targetReg, op1Reg, addss/sd targetReg, Op2Reg

        if (op1->IsCnsNonZeroFltOrDbl())
        {
            MakeSrcContained(node, op1);
        }
        else if (IsContainableMemoryOp(op1))
        {
            isSafeToContainOp1 = IsSafeToContainMem(node, op1);
            if (isSafeToContainOp1)
            {
                MakeSrcContained(node, op1);
            }
        }
    }

    if (!op1->isContained() && !op2->isContained())
    {
        // If there are no containable operands, we can make an operand reg optional.
        isSafeToContainOp1 = IsSafeToMarkRegOptional(node, op1);
        isSafeToContainOp2 = IsSafeToMarkRegOptional(node, op2);
        SetRegOptionalForBinOp(node, isSafeToContainOp1, isSafeToContainOp2);
    }
}

#endif // TARGET_XARCH
