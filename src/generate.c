#include "include/stackmap.h"
#include "include/hash_table.h"
#include <stdbool.h>

inline bool isBasePointer(value_location_t* first, value_location_t* second) {
    return first->kind == second->kind 
           && first->offset == second->offset;
}

inline bool isIndirect(value_location_t* p) {
    return p->kind == Indirect;
}

frame_info_t* generate_frame_info(callsite_header_t* callsite, function_info_t* fn) {
    uint64_t retAddr = fn->address + callsite->codeOffset;
    uint64_t frameSize = fn->stackSize;
    
    // now we parse the location array according to the specific type 
    // of locations that statepoints emit: 
    // http://llvm.org/docs/Statepoints.html#stack-map-format
    
    uint16_t numLocations = callsite->numLocations;
    value_location_t* locations = (value_location_t*)(callsite + 1);
    
    // the first 2 locations are constants we dont care about, but if asserts are
    // on we check that they're constants.
    for(uint16_t i = 0; i < 2; i++) {
        assert(locations->kind == Constant 
            && "first 3 locations must be constants in statepoint stackmaps");
        locations++;
        numLocations--;
    }
    
    // the 3rd constant describes the number of "deopt" parameters
    // that we should skip over.
    assert(locations->kind == Constant && "this should be a constant");
    int32_t numDeopt = locations->offset;
    locations++;
    numLocations--;
    
    assert(numDeopt >= 0 && "unexpected negative here");
    locations += numDeopt; 
    numLocations -= numDeopt;
    
    /* 
       The remaining locations describe pointer that the GC should track, and use a special
       format:
       
       "Each record consists of a pair of Locations. The second element in the record
       represents the pointer (or pointers) which need updated. The first element in the
       record provides a pointer to the base of the object with which the pointer(s) being
       relocated is associated. This information is required for handling generalized
       derived pointers since a pointer may be outside the bounds of the original
       allocation, but still needs to be relocated with the allocation."

       NOTE that we are currently ignoring the following part of the documentation because
       it doesn't make sense... locations have no size field:

       "The Locations within each record may [be] a multiple of pointer size. In the later
       case, the record must be interpreted as describing a sequence of pointers and their
       corresponding base pointers. If the Location is of size N x sizeof(pointer), then
       there will be N records of one pointer each contained within the Location. Both
       Locations in a pair can be assumed to be of the same size."
    */
    
    
    assert((numLocations & 2) == 0 && "all of the pointer locations come in pairs!");
    uint16_t numSlots = numLocations / 2; 
    
    frame_info_t* frame = malloc(size_of_frame(numSlots));
    frame->retAddr = retAddr;
    frame->frameSize = frameSize;
    frame->numSlots = numSlots;
    
    // now to initialize the slots, we need to make two passes in order to put
    // base pointers first, then derived pointers.
    
    uint16_t numBasePtrs = 0;
    pointer_slot_t* currentSlot = frame->slots;
    for(uint16_t i = 0; i < numSlots; i++, locations += 2) {
        value_location_t* base = (value_location_t*)(locations);
        value_location_t* derived = (value_location_t*)(locations + 1);
        
        // we check that all locations are indirects.
        if( ! (isIndirect(base) && isIndirect(derived)) ) {
            // it is expected that all pointers were saved to the stack and
            // are not in a register!
            exit(EXIT_FAILURE);
        }
        
        if( ! isBasePointer(base, derived)) {
            continue;
        }
        
        // it's a base pointer, aka base is equivalent to derived.
        // save the info.
        pointer_slot_t newSlot;
        newSlot.kind = -1;
        newSlot.offset = base->offset;
        *currentSlot = newSlot;
        
        // get ready for next iteration
        numBasePtrs++;
        currentSlot++;
    }
    
    // now we do the derived pointers. we already know all locations are indirects now.
    
    pointer_slot_t* processedBase = frame->slots;
    for(uint16_t i = 0; i < numSlots; i++, locations += 2) {
        value_location_t* base = (value_location_t*)(locations);
        value_location_t* derived = (value_location_t*)(locations + 1);
        
        if(isBasePointer(base, derived)) {
            // already processed
            continue;
        }
        
        // find the index in our frame corresponding to the base pointer.
        uint16_t baseIdx;
        bool found = false;
        for(uint16_t k = 0; k < numBasePtrs; k++) {
            if(processedBase[k].offset == base->offset) {
                found = true;
                baseIdx = k;
                break;
            }
        }
        
        if(!found) {
            // something's gone awry, let's bail!
            exit(EXIT_FAILURE);
        }
        
        // save the derived pointer's info
        pointer_slot_t newSlot;
        newSlot.kind = baseIdx;
        newSlot.offset = derived->offset;
        *currentSlot = newSlot;
        
        // new iteration
        currentSlot++;
    }

    return frame;
}

callsite_header_t* next_callsite(callsite_header_t* callsite) {
    uint16_t numLocations = callsite->numLocations;
    
    // skip over locations
    value_location_t* locations = (value_location_t*)(callsite + 1);
    locations += numLocations;
    
    liveout_header_t* liveout_header = (liveout_header_t*)locations;
    uint16_t numLiveouts = liveout_header->numLiveouts;
    
    // skip over liveouts
    liveout_location_t* liveouts = (liveout_location_t*)(liveout_header + 1);
    liveouts += numLiveouts;
    
    // realign pointer to 8 byte alignment.
    uint64_t ptr_val = (uint64_t)liveouts;
    ptr_val = (ptr_val + 7) & ~0x7;
    
    return (callsite_header_t*)ptr_val;
}

statepoint_table_t* generate_table(void* map, float load_factor) {
    stackmap_header_t* header = (stackmap_header_t*)map;
    uint64_t numCallsites = header->numRecords;
    
    statepoint_table_t* table = new_table(load_factor, numCallsites);
    
    function_info_t* functions = (function_info_t*)(header + 1);
    
    // we skip over constants, which are uint64_t's
    callsite_header_t* callsite = 
        (callsite_header_t*)(
            ((uint64_t*)(functions + header->numFunctions)) + header->numConstants
        );
    
    
    function_info_t* currentFn = functions;
    uint32_t visited = 0;
    for(uint64_t _unused = 0; _unused < numCallsites; _unused++) {
        if(visited >= currentFn->callsiteCount) {
            currentFn++;
            visited = 0;
        }

        frame_info_t* info = generate_frame_info(callsite, currentFn);
        
        insert_key(table, info->retAddr, info);
        
        // setup next iteration
        callsite = next_callsite(callsite);
        visited++;
    }
    
    return table;
}