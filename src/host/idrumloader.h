// IDrumLoader — host-discoverable file-loading interface for DRUMku, plus the
// shared parameter-ID layout the GUI-less host needs.
//
// VST3 parameters are normalized floats only, so a sample (.wav) path cannot
// travel as a parameter. DRUMku is a drum rack of N slots; each slot loads one
// .wav sample. This interface, implemented by DRUMku's edit controller and
// discovered by hosts via queryInterface, carries the per-slot sample path. A
// host that finds it can offer per-slot file pickers; a host that does not know
// it is unaffected. The controller forwards the path (with its slot index) to
// the processor over the SDK's IConnectionPoint message channel, and the paths
// persist in the plug-in state.
//
// Everything else about a slot — its volume, its assigned MIDI note, and how
// many slots the rack shows — is expressed as ordinary VST3 parameters (see the
// ID layout below), so a generic host renders them without knowing this
// interface. The host-side copy of this header must stay in sync with the one
// under DRUMku/source/.

#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace DRUMku
{

// Maximum number of loader slots. The rack shows the first `slot count`
// (kSlotCountId) of these; the rest stay silent. Volume/Note parameters for all
// slots are declared up front so their IDs never change — this keeps the
// parameter set fixed (VST3-safe) while the visible rack grows via the Add
// button. Never lower this after a release; states embed slot data by index.
static const Steinberg::int32 kMaxSlots = 64;

// How many rack rows are shown on a fresh instance (before any +Add). The
// processor and controller must agree on this so a state saved without ever
// touching the "Slots" control restores the same row count it displayed.
static const Steinberg::int32 kDefaultSlotCount = 10;

// Parameter IDs. Never change these after a release — projects embed them.
enum ParamIDs : Steinberg::Vst::ParamID {
    kBypassId = 100,    // toggle: silence output when on
    kSlotCountId = 101, // 1 .. kMaxSlots, default 10 — how many rows the rack shows

    // Per-slot parameter bases: slot i's parameter has ID (base + i).
    kSlotVolumeBase = 1000, // linear gain 0 .. 1, default 0.8
    kSlotNoteBase = 2000,   // assigned MIDI note: plain 0..127, plain 128 = unassigned

    // Hidden, read-only, transient (never persisted): the processor pulses the
    // note-on velocity here through the output parameter changes so an editor
    // can flash the pad. Also the return channel for plug-in-side MIDI learn —
    // a captured note arrives as an output change on (kSlotNoteBase + slot).
    kSlotActivityBase = 3000, // last trigger velocity 0 .. 1
};

// A slot's Note parameter is a stepped range 0..kNoteUnassigned; the top step
// (128) means "no note bound". Plain value maps to MIDI pitch directly.
static const Steinberg::int32 kNoteUnassigned = 128;

//------------------------------------------------------------------------
class IDrumLoader : public Steinberg::FUnknown
{
public:
    // Set / clear the .wav sample for a slot (absolute path, UTF-8).
    // nullptr or "" clears the slot. Out-of-range slot returns kInvalidArgument.
    virtual Steinberg::tresult PLUGIN_API setSampleFile(Steinberg::int32 slot,
                                                        const Steinberg::char8 *path) = 0;

    // Get the current path for a slot (UTF-8, empty when nothing is loaded).
    // Returns kResultFalse if the buffer is too small, kInvalidArgument if slot
    // is out of range.
    virtual Steinberg::tresult PLUGIN_API getSampleFile(Steinberg::int32 slot,
                                                        Steinberg::char8 *buffer,
                                                        Steinberg::int32 bufferSize) = 0;

    static const Steinberg::FUID iid;
};

DECLARE_CLASS_IID(IDrumLoader, 0xDA1B9116, 0x2459CFA8, 0xC3A3B121, 0xEF2657DA)

} // namespace DRUMku
