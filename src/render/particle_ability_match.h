#pragma once
#include <array>
#include <cctype>
#include <string_view>
#include <algorithm>
#include <cstring>
#include "particle_item_aliases.h"
#include "particle_source_catalog.h"

namespace particle_ability_match {
inline std::string_view canonical_resource(std::string_view resource) {
    if(resource.ends_with(".vpcf_c"))resource.remove_suffix(2);
    struct query_cache {std::array<char,384> name{};std::size_t size=~std::size_t(0);const char* replacement=nullptr;};
    thread_local query_cache cache;
    if(cache.size==resource.size()&&std::memcmp(cache.name.data(),resource.data(),resource.size())==0)
        return cache.replacement?std::string_view(cache.replacement):resource;
    const auto& aliases=particle_catalog::aliases;
    const auto at=std::lower_bound(std::begin(aliases),std::end(aliases),resource,
        [](const auto& entry,std::string_view key){return std::string_view(entry.resource)<key;});
    const char* replacement=at!=std::end(aliases)&&std::string_view(at->resource)==resource?at->source:nullptr;
    if(resource.size()<cache.name.size()) {
        std::memcpy(cache.name.data(),resource.data(),resource.size());cache.size=resource.size();cache.replacement=replacement;
    }
    return replacement?std::string_view(replacement):resource;
}
// UI particles may carry a hero owner, but do not disclose its position.
// Check basename too: depot/cosmetic paths can omit the usual UI directory.
inline bool local_feedback(std::string_view name) {
    name=canonical_resource(name);
    constexpr std::string_view directories[]{"/ui_mouseactions/","/ui/","/hud/",
        "/dashboard/","/portrait","/loadout/"};
    for(const auto path:directories)if(name.find(path)!=std::string_view::npos)return true;
    const auto at=name.find_last_of("/\\");
    if(at!=std::string_view::npos)name.remove_prefix(at+1);
    name=name.substr(0,name.find('.'));
    // Portrait/loadout copies can use gameplay folders and hero prefixes.
    constexpr std::string_view previews[]{"_portrait","_portait","_loadout"};
    for(const auto word:previews)if(name.find(word)!=std::string_view::npos)return true;
    constexpr std::string_view prefixes[]{"select_unit","selection_","unit_selection",
        "clicked_","drag_select","camera_focus_","hero_highlighter","hero_ring",
        "hero_underglow","select_hero","selected_unit","ping_",
        "mouseover_","mouse_over_","hover_","range_display","range_finder"};
    for(const auto prefix:prefixes)if(name.starts_with(prefix))return true;
    return false;
}
inline std::string_view resource_hero(std::string_view resource) {
    resource=canonical_resource(resource);
    const auto& folders=particle_catalog::hero_folders;
    for(auto end=resource.find_last_of('/');end!=std::string_view::npos;) {
        const auto folder=resource.substr(0,end);
        const auto entry=std::lower_bound(std::begin(folders),std::end(folders),folder,
            [](const auto& binding,std::string_view key){return std::string_view(binding.resource)<key;});
        if(entry!=std::end(folders)&&std::string_view(entry->resource)==folder)return entry->source;
        end=folder.find_last_of('/');
    }
    return {};
}
inline unsigned behavior(std::string_view name) {
    const auto& entries=particle_catalog::sources;
    const auto at=std::lower_bound(std::begin(entries),std::end(entries),name,
        [](const auto& entry,std::string_view key){return std::string_view(entry.name)<key;});
    return at!=std::end(entries)&&std::string_view(at->name)==name?at->flags:0;
}
inline bool base_attack(std::string_view resource,std::string_view hero) {
    resource=canonical_resource(resource);
    for(const auto& entry:particle_catalog::attacks)
        if(resource==entry.resource&&hero==entry.source)return true;
    return false;
}
// Only caster-carried aura/toggle/self-buff emitters are body evidence. Target debuffs,
// travelling projectiles and remote impacts disclose the effect, not its caster.
inline bool caster_emitter(std::string_view resource,std::string_view ability) {
    resource=canonical_resource(resource);
    const auto flags=behavior(ability);
    const bool continuous=(flags&particle_catalog::aura)||
        ((flags&particle_catalog::toggle)&&(flags&particle_catalog::no_target));
    const auto at=resource.find_last_of("/\\");
    if(at!=std::string_view::npos)resource.remove_prefix(at+1);
    constexpr std::string_view remote[]{"target","enemy","friend","debuff","impact","hit",
        "projectile","attack","trail","launch"};
    for(const auto word:remote)if(resource.find(word)!=std::string_view::npos)return false;
    if(continuous)return true;
    // No-target alone is insufficient: ground novas are also no-target.
    // Require self-buff semantics in addition to a verified carrier and CP0.
    if(!(flags&particle_catalog::no_target) ||
        (flags&(particle_catalog::point|particle_catalog::unit_target|particle_catalog::passive)))return false;
    constexpr std::string_view self[]{"buff","shield","orbit","_active","_self","_body"};
    for(const auto word:self)if(resource.find(word)!=std::string_view::npos)return true;
    return false;
}
inline std::array<char, 192> normalized(std::string_view text) {
    std::array<char, 192> result{};
    std::size_t size = 0;
    for (const unsigned char value : text) {
        if (std::isalnum(value) && size + 1 < result.size())
            result[size++] = static_cast<char>(std::tolower(value));
    }
    return result;
}
inline int modifier_score(std::string_view class_name,std::string_view ability) {
    const auto type_key=normalized(class_name),ability_key=normalized(ability);
    const std::string_view type(type_key.data()),name(ability_key.data());
    if(type.find("cdotamodifier")==std::string_view::npos)return 0;
    if(name.size()>=6&&type.find(name)!=std::string_view::npos)return int(name.size())+100;
    if(ability.starts_with("item_")) {
        const auto short_key=normalized(ability.substr(5));
        const std::string_view item(short_key.data());
        if(item.size()>=6&&type.find(item)!=std::string_view::npos)return int(item.size())+80;
    }
    return 0;
}
inline bool token_match(std::string_view text,std::string_view key) {
    for(std::size_t at=0;at<text.size();) {
        while(at<text.size()&&!std::isalnum(static_cast<unsigned char>(text[at])))++at;
        const auto begin=at;
        while(at<text.size()&&std::isalnum(static_cast<unsigned char>(text[at])))++at;
        const auto value=normalized(text.substr(begin,at-begin));
        if(std::string_view(value.data())==key)return true;
    }
    return false;
}
// Candidates come from the owning hero's live abilities and inventory.
// A generic particle word such as "hit" cannot select an arbitrary spell.
inline int score(std::string_view particle, std::string_view ability,
    std::string_view hero) {
    particle=canonical_resource(particle);
    if(local_feedback(particle))return 0;
    for(const auto& entry:particle_catalog::resources)
        if(particle==entry.resource&&ability==entry.source)return 120;
    const bool hero_path=resource_hero(particle)==hero;
    const auto separator = particle.find_last_of("/\\");
    if (separator != std::string_view::npos) particle.remove_prefix(separator + 1);
    particle = particle.substr(0, particle.find('.'));
    const auto particle_key = normalized(particle);
    const auto ability_key = normalized(ability);
    const std::string_view key(particle_key.data());
    const std::string_view full(ability_key.data());
    if (full.size() >= 6 && key.find(full) != std::string_view::npos) return 100;
    if (ability.starts_with("item_")) {
        const auto short_name=normalized(ability.substr(5));
        const std::string_view item_key(short_name.data());
        if (item_key.size()>=6 && key.find(item_key)!=std::string_view::npos) return 90;
        if(item_key.size()>=3 && ability.substr(5).find('_')==std::string_view::npos) {
            for(std::size_t at=0;at<particle.size();) {
                while(at<particle.size()&&!std::isalnum(static_cast<unsigned char>(particle[at])))++at;
                const auto begin=at;
                while(at<particle.size()&&std::isalnum(static_cast<unsigned char>(particle[at])))++at;
                const auto token=normalized(particle.substr(begin,at-begin));
                if(std::string_view(token.data())==item_key)return 90;
            }
        }
        const auto& aliases=particle_item_aliases::entries;
        const auto found=std::lower_bound(std::begin(aliases),std::end(aliases),ability,
            [](const auto& entry,std::string_view name) { return std::string_view(entry.name)<name; });
        if (found!=std::end(aliases) && std::string_view(found->name)==ability &&
            key.find(found->alias)!=std::string_view::npos) return 90;
        return 0;
    }
    constexpr std::string_view hero_prefix = "npc_dota_hero_";
    if (hero.starts_with(hero_prefix)) hero.remove_prefix(hero_prefix.size());
    if (!hero.empty() && ability.starts_with(hero) && ability.size() > hero.size() &&
        ability[hero.size()] == '_') ability.remove_prefix(hero.size() + 1);
    else return 0;
    const auto suffix_key = normalized(ability);
    const std::string_view suffix(suffix_key.data());
    if(suffix.size()>=6&&key.find(suffix)!=std::string_view::npos)return 80;
    // Short spell names are valid only as complete basename tokens; "rot"
    // must not match "rotation". Ownership already restricts the candidates.
    if(ability.size()>=3&&ability.find('_')==std::string_view::npos) {
        for(std::size_t at=0;at<particle.size();) {
            while(at<particle.size()&&!std::isalnum(static_cast<unsigned char>(particle[at])))++at;
            const auto begin=at;
            while(at<particle.size()&&std::isalnum(static_cast<unsigned char>(particle[at])))++at;
            const auto token=normalized(particle.substr(begin,at-begin));
            if(std::string_view(token.data())==suffix)return 80;
        }
    }
    // Valve particle basenames often omit one word from the ability name:
    // death_pulse -> pulse, heartstopper_aura -> heartstopper. Keep ownership
    // and the hero namespace mandatory; the caller rejects equal candidates.
    const auto hero_key=normalized(hero);
    const bool hero_basename=!hero.empty()&&key.starts_with(hero_key.data());
    if(hero_path||hero_basename) {
        int best=0;
        for(std::size_t begin=0;begin<ability.size();) {
            const auto end=ability.find('_',begin);
            const auto token=ability.substr(begin,end==std::string_view::npos?ability.size()-begin:end-begin);
            constexpr std::string_view generic[]{"aura","attack","damage","bonus","passive","effect","target"};
            if(token.size()>=4&&std::find(std::begin(generic),std::end(generic),token)==std::end(generic)&&
                token_match(particle,token))best=std::max(best,40+int(std::min(token.size(),std::size_t(20))));
            if(end==std::string_view::npos)break;
            begin=end+1;
        }
        return best;
    }
    return 0;
}
}
