pub mod abi;
mod catalog;
mod creeps;
mod lanes;
pub mod world;
use abi::*;
use std::{
    mem::{align_of, size_of},
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Mutex, OnceLock},
};
static WORLD: OnceLock<Mutex<world::World>> = OnceLock::new();

/// Creation-time decision. Try-lock only: a game frame never waits for replay.
/// # Safety
/// Host must provide one aligned, readable numeric query for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn askermap_resolve_attachment(query: *const AttachmentQuery) -> u32 {
    if query.is_null() || !(query as usize).is_multiple_of(align_of::<AttachmentQuery>()) {
        return 1;
    }
    catch_unwind(|| {
        // SAFETY: the host owns pointer validity, as on the process boundary.
        let q = unsafe { &*query };
        if q.size != size_of::<AttachmentQuery>() as u32
            || q.version != ABI
            || !q.model.valid()
            || !q.attachment.valid()
            || !q.origin.valid()
        {
            return 1;
        }
        let resource = text(&q.resource);
        let hero = text(&q.hero);
        let Some(world) = WORLD.get() else {
            return 2;
        };
        let Ok(world) = world.try_lock() else {
            return 2;
        };
        if !world.attachment_is_frozen(q.actor, q.model) {
            return 2;
        }
        drop(world);
        if (q.origin.x.abs() <= 1. && q.origin.y.abs() <= 1.)
            || catalog::feedback(resource)
            || catalog::resource_hero(resource) != Some(hero)
            || (q.model.x - q.attachment.x).hypot(q.model.y - q.attachment.y) > 64.
            || (q.origin.x - q.attachment.x).hypot(q.origin.y - q.attachment.y) < 128.
        {
            return 2;
        }
        let matcher = catalog::ResourceMatch::new(resource);
        let mut best = 0;
        let mut tied = false;
        for (ability, _) in catalog::SOURCES {
            if !catalog::self_cast(resource, ability) {
                continue;
            }
            let score = matcher.score(ability, &catalog::normalized(ability));
            if score > best {
                best = score;
                tied = false;
            } else if score > 0 && score == best {
                tied = true;
            }
        }
        if best > 0 && !tied { 0 } else { 2 }
    })
    .unwrap_or(3)
}

#[unsafe(no_mangle)]
pub extern "C" fn askermap_layout(which: u32) -> u32 {
    match which {
        0 => ABI,
        1 => size_of::<Frame>() as u32,
        2 => size_of::<Observation>() as u32,
        3 => size_of::<Snapshot>() as u32,
        _ => 0,
    }
}
/// # Safety
/// The host supplies live, aligned, non-overlapping pointers for this call.
/// `events` addresses `count` initialized Observation values; `out` is writable.
/// Invalid/unmapped pointers cannot be validated portably by an FFI library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn askermap_process(
    frame: *const Frame,
    events: *const Observation,
    count: usize,
    out: *mut Snapshot,
) -> u32 {
    if frame.is_null()
        || out.is_null()
        || count > MAX_EVENTS
        || (!events.is_null() && !(events as usize).is_multiple_of(align_of::<Observation>()))
        || !(frame as usize).is_multiple_of(align_of::<Frame>())
        || !(out as usize).is_multiple_of(align_of::<Snapshot>())
        || (count > 0 && events.is_null())
    {
        return 1;
    }
    // SAFETY: host pointer/length/lifetime contract above; null events with 0
    // count deliberately avoid constructing a slice from a null pointer.
    let (frame, events) = unsafe {
        (
            &*frame,
            if count == 0 {
                &[]
            } else {
                std::slice::from_raw_parts(events, count)
            },
        )
    };
    if frame.size != size_of::<Frame>() as u32
        || frame.version != ABI
        || frame.local_team < 2
        || frame.local_team > 3
    {
        return 2;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut world = WORLD
            .get_or_init(|| Mutex::new(world::World::default()))
            .lock()
            .map_err(|_| 3u32)?;
        world.advance(frame, events)
    }));
    match result {
        Ok(Ok(snapshot)) => {
            // SAFETY: `out` follows the writable host contract.
            unsafe { out.write(snapshot) };
            0
        }
        Ok(Err(code)) => code,
        Err(_) => 4,
    }
}
