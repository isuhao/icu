//
//  rbbitblb.h
//

/*
**********************************************************************
*   Copyright (c) 2002, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*/

#ifndef RBBITBLB_H
#define RBBITBLB_H

#include "unicode/utypes.h"
#include "unicode/uobject.h"
#include "unicode/rbbi.h"
#include "rbbinode.h"


U_NAMESPACE_BEGIN

class RBBIRuleScanner;
class RBBIRuleBuilder;

//
//  class RBBITableBuilder is part of the RBBI rule compiler.
//                         It builds the state transition table used by the RBBI runtime
//                         from the expression syntax tree generated by the rule scanner.
//
//                         This class is part of the RBBI implementation only.
//                         There is no user-visible public API here.
//

class RBBITableBuilder : public UObject {
public:
    RBBITableBuilder(RBBIRuleBuilder *rb, RBBINode **rootNode);
    ~RBBITableBuilder();

    void     build();
    int32_t  getTableSize();            // Return the runtime size in bytes of
                                        //     the built state table
    void     exportTable(void *where);  // fill in the runtime state table.
                                        //     Sufficient memory must exist at
                                        //     the specified location.

    //  TODO:  add getter function(s) for the built table.

    /**
     * ICU "poor man's RTTI", returns a UClassID for the actual class.
     *
     * @draft ICU 2.2
     */
    virtual inline UClassID getDynamicClassID() const { return getStaticClassID(); }

    /**
     * ICU "poor man's RTTI", returns a UClassID for this class.
     *
     * @draft ICU 2.2
     */
    static inline UClassID getStaticClassID() { return (UClassID)&fgClassID; }

private:
    void     calcNullable(RBBINode *n);
    void     calcFirstPos(RBBINode *n);
    void     calcLastPos(RBBINode  *n);
    void     calcFollowPos(RBBINode *n);
    void     buildStateTable();
    void     flagAcceptingStates();
    void     flagLookAheadStates();
    void     flagTaggedStates();

    // Set functions for UVector.
    //   TODO:  make a USet subclass of UVector

    void     setAdd(UVector *dest, UVector *source);
    UBool    setEquals(UVector *a, UVector *b);

    void     printSet(UVector *s);
    void     printPosSets(RBBINode *n = NULL);
    void     printStates();


private:
    RBBIRuleBuilder  *fRB;
    RBBINode         *&fTree;              // The root node of the parse tree to build a
                                           //   table for.
    UErrorCode       *fStatus;

    UVector          *fDStates;            //  D states (Aho's terminology)
                                           //  Index is state number
                                           //  Contents are RBBIStateDescriptor pointers.

    /**
     * The address of this static class variable serves as this class's ID
     * for ICU "poor man's RTTI".
     */
    static const char fgClassID;
};

//
//  RBBIStateDescriptor - The DFA is constructed as a set of these descriptors,
//                        one for each state.
class RBBIStateDescriptor : public UObject {
public:
    UBool            fMarked;
    int32_t          fAccepting;
    int32_t          fLookAhead;
    int32_t          fTagVal;
    UVector          *fPositions;          // Set of parse tree positions associated
                                           //   with this state.  Unordered (it's a set).
                                           //   UVector contents are RBBINode *

    UVector          *fDtran;              // Transitions out of this state.
                                           //   indexed by input character
                                           //   contents is int index of dest state
                                           //   in RBBITableBuilder.fDStates

    RBBIStateDescriptor(int maxInputSymbol,  UErrorCode *fStatus);
    ~RBBIStateDescriptor();

    /**
     * ICU "poor man's RTTI", returns a UClassID for the actual class.
     *
     * @draft ICU 2.2
     */
    virtual inline UClassID getDynamicClassID() const { return getStaticClassID(); }

    /**
     * ICU "poor man's RTTI", returns a UClassID for this class.
     *
     * @draft ICU 2.2
     */
    static inline UClassID getStaticClassID() { return (UClassID)&fgClassID; }

private:

    /**
     * The address of this static class variable serves as this class's ID
     * for ICU "poor man's RTTI".
     */
    static const char fgClassID;
};



U_NAMESPACE_END
#endif
