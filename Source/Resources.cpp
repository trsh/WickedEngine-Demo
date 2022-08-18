#include "Resources.h"
#include <Utility/DirectXMath.h>
#include <memory>
#include <mutex>
#include <set>
#include <wiArchive.h>
#include <wiECS.h>
#include <wiHelper.h>
#include <wiJobSystem.h>
#include <wiScene.h>
#include <wiUnorderedMap.h>
#include <wiUnorderedSet.h>

using namespace Game::Resources;

void Library::Instance::Init(wi::jobsystem::context* joblist){
    if(joblist == nullptr) joblist = &scene->job_scenestream;
    
    wi::jobsystem::Execute(*joblist, [=](wi::jobsystem::JobArgs args_job){
        std::scoped_lock lock (scene->mutex_scenestream);

        auto find_collection = scene->collections.find(wi::helper::string_hash(file.c_str()));
        bool do_clone = false;
        if(find_collection == scene->collections.end()) // Load from disk if collection does not exist!
        {
            // Check loading strategy first
            if(strategy == LOAD_DIRECT){
                Scene load_scene;
                auto prototype_root = wi::scene::LoadModel(load_scene.wiscene, file, XMMatrixIdentity(), true);

                // Scan the loaded scene and enlist entities
                load_scene.wiscene.FindAllEntities(entities);
                entities.erase(prototype_root);

                // Merge and set parent to collection
                scene->wiscene.Merge(load_scene.wiscene);
                scene->wiscene.Component_Attach(prototype_root, instance_id);
                scene->wiscene.Entity_Remove(prototype_root, false);

                // Store newly made collection to the list
                scene->collections[wi::helper::string_hash(file.c_str())] = instance_id;

                // If we're loading as library we have to stash non-data objects!
                if(type == LIBRARY){
                    for(auto& entity : entities){
                        bool ignore = (
                            (scene->wiscene.materials.Contains(entity) == true) ||
                            (scene->wiscene.meshes.Contains(entity) == true)
                        );

                        if(!ignore){ // Stash away non data objects by disabling them!
                            scene->Entity_Disable(entity);
                        }
                    }
                }

                collection_id = instance_id;
            } else {
                // Spawn another entity as library and then ask them to load it as library
                auto lib_entity = scene->CreateInstance("LIB_"+file);
                auto lib_instance = scene->instances.GetComponent(lib_entity);

                wi::jobsystem::context preload_list;

                lib_instance->scene = scene;
                lib_instance->instance_id = lib_entity;
                lib_instance->strategy = LOAD_DIRECT;
                lib_instance->type = LIBRARY;

                lib_instance->Init(&preload_list);
                wi::jobsystem::Wait(preload_list);

                do_clone = true;
            }
        }
        else do_clone = true;
        
        // We can just clone if the collection exists
        if(do_clone)
        { 
            auto collection_entity = find_collection->second;
            auto collection_instance = scene->instances.GetComponent(collection_entity);

            if(collection_instance != nullptr) // Dear sir, we have to make sure we're safe from crashes!
            {
                wi::ecs::Entity prototype_root;
                
                if(entity_name == "") // If there's no entity name specified, this means we clone the entire instance!
                { 
                    wi::ecs::EntitySerializer seri;
                    auto prototype_root = scene->Entity_Clone(collection_entity, seri);
                    
                    // Restore stashed components!
                    if(collection_instance->type == LIBRARY) 
                    { 
                        for(auto& entity : collection_instance->entities){
                            auto find_disabled = scene->disabled_list.find(entity);
                            if(find_disabled != scene->disabled_list.end())
                            {
                                auto sub_proto_root = scene->Entity_Clone(entity, seri);
                                scene->wiscene.Component_Attach(sub_proto_root, instance_id);
                            }
                        }
                    }

                    // Remap and store entity list!
                    for (auto& entity : collection_instance->entities)
                    {
                        entities.insert(seri.remap[entity]);
                    }
                } 
                else 
                {
                    bool clone = false;
                    wi::ecs::EntitySerializer seri;

                    // Find the entity name, if exist then clone!
                    for(auto& entity : collection_instance->entities) 
                    {
                        auto nameComponent = scene->wiscene.names.GetComponent(entity);
                        auto find_disabled = scene->disabled_list.find(entity);

                        // If it is disabled then we need to find the name on the disabled component instead!
                        if(find_disabled != scene->disabled_list.end()) 
                        {
                            auto disabledNameStore = scene->wiscene.names.GetComponent(find_disabled->second);
                            if(disabledNameStore != nullptr)
                            {
                                if(disabledNameStore->name == entity_name) clone = true;
                            }
                        } 
                        else if (nameComponent != nullptr) // Else if we find the entity then clone it!
                        {
                            if(nameComponent->name == entity_name) clone = true;
                        }

                        if(clone){
                            // Clone and then parent to instance
                            prototype_root = scene->Entity_Clone(entity, seri);
                            scene->wiscene.Component_Attach(prototype_root, instance_id);
                            
                            collection_id = collection_entity;
                            entities.insert(seri.remap[entity]);

                            break;
                        }
                    }
                }
            }
        }

        // For streaming objects, we need to track transition fx,
        // so first we need to register objectComponents of the instance
        // And then start the transition from zero!
        auto streamComponent = scene->streams.GetComponent(instance_id);
        if(streamComponent != nullptr)
        {
            for (auto& entity : entities){
                auto objectComponent = scene->wiscene.objects.GetComponent(entity);
                if(objectComponent != nullptr) streamComponent->instance_original_transparency[entity] = objectComponent->GetTransparency();
            }
        }
    });
}

void Library::Instance::Unload(){
    for(auto& entity : entities){
        auto hierarchyComponent = scene->wiscene.hierarchy.GetComponent(entity);

        if(hierarchyComponent != nullptr)
        {
            if(hierarchyComponent->parentID == instance_id)
            {
                scene->wiscene.Entity_Remove(entity);
            }
        }
    }
}

void Library::Instance::Serialize(wi::Archive& archive, wi::ecs::EntitySerializer& seri){
    if(archive.IsReadMode())
    {
        archive >> file;
        archive >> entity_name;
    }
    else
    {
        archive << file;
        archive << entity_name;
    }
}



void Library::Disabled::Serialize(wi::Archive& archive, wi::ecs::EntitySerializer& seri){
    wi::ecs::SerializeEntity(archive, entity, seri);
}



void Library::Stream::Serialize(wi::Archive &archive, wi::ecs::EntitySerializer &seri){
    wi::ecs::SerializeEntity(archive, substitute_object, seri);
    stream_zone.Serialize(archive, seri);
}



// ScriptObjectData initializers exists within Resources_BindLua.cpp

Library::ScriptObjectData::~ScriptObjectData(){
    Unload();
}

void Library::ScriptObjectData::Serialize(wi::Archive &archive, wi::ecs::EntitySerializer &seri){
    if(archive.IsReadMode())
    {
        archive >> file;
        archive >> properties;
    }
    else
    {
        archive << file;
        archive << properties;
    }
}



void Library::ScriptObject::Init(){
    for(auto& script : scripts){
        script.Init();
    }
}

void Library::ScriptObject::Unload(){
    for(auto& script : scripts){
        script.Unload();
    }
}

Library::ScriptObject::~ScriptObject(){
    Unload();
}

void Library::ScriptObject::Serialize(wi::Archive &archive, wi::ecs::EntitySerializer &seri){
    for(auto& script : scripts){
        script.Serialize(archive, seri);
    }
}



wi::ecs::Entity Scene::CreateInstance(std::string name){
    auto entity = wi::ecs::CreateEntity();

    auto& namecomponent = wiscene.names.Create(entity);
    namecomponent.name = name;

    instances.Create(entity);

    return entity;
}

void Scene::SetStreamable(wi::ecs::Entity entity, bool set, wi::primitive::AABB bound){
    if(set)
    {
        auto& streamcomponent = streams.Create(entity);
        streamcomponent.stream_zone = bound;
    }
    else
    {
        if(streams.Contains(entity))
        {
            streams.Remove(entity);
        }
    }
}

void Scene::SetScript(wi::ecs::Entity entity, bool set, std::string file){
    if(set)
    {
        auto& scriptcomponent = scriptobjects.Create(entity);
        if(file != "")
        {
            auto& scriptdata = scriptcomponent.scripts.emplace_back();
            scriptdata.file = file;
        }
    }
    else
    {
        if(scriptobjects.Contains(entity))
        {
            scriptobjects.Remove(entity);
        }
    }
}

void Scene::Entity_Disable(wi::ecs::Entity entity){
    auto disable_handle = wi::ecs::CreateEntity();
    auto& disabledNameStore = wiscene.names.Create(disable_handle); 
    auto& disabledComponent = disabled.Create(disable_handle);

    wi::ecs::EntitySerializer seri;
 
    auto nameComponent = wiscene.names.GetComponent(entity);
    disabledNameStore.name = nameComponent->name;

    disabledComponent.entity_store.SetReadModeAndResetPos(false);
    wiscene.Entity_Serialize(disabledComponent.entity_store, seri, entity,
        wi::scene::Scene::EntitySerializeFlags::RECURSIVE | wi::scene::Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);

    disabled_list[entity] = disable_handle;
    disabledComponent.remap = seri.remap;

    wiscene.Entity_Remove(entity);
}

void Scene::Entity_Enable(wi::ecs::Entity entity){
    auto find_disabled = disabled_list.find(entity);
    if(find_disabled != disabled_list.end())
    {
        auto disabledComponent = disabled.GetComponent(find_disabled->second);

        wi::ecs::EntitySerializer seri;
        seri.remap = disabledComponent->remap;
        disabledComponent->entity_store.SetReadModeAndResetPos(true);
        wiscene.Entity_Serialize(disabledComponent->entity_store, seri, wi::ecs::INVALID_ENTITY,
            wi::scene::Scene::EntitySerializeFlags::RECURSIVE | wi::scene::Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);

        wiscene.Entity_Remove(find_disabled->second);
    }
}

wi::ecs::Entity Scene::Entity_Clone(wi::ecs::Entity entity, wi::ecs::EntitySerializer& seri){
    wi::Archive* archive;

    wi::Archive new_archive;
    auto find_disabled = disabled_list.find(entity);
    if(find_disabled != disabled_list.end()) // If disabled then just clone the disabled data
    {
        auto disabledComponent = disabled.GetComponent(find_disabled->second);
        archive = &disabledComponent->entity_store;
        seri.remap = disabledComponent->remap;
    }
    else // If not then just copy it as usual
    {
        archive = &new_archive;
        archive->SetReadModeAndResetPos(false);
        wiscene.Entity_Serialize(*archive, seri, entity, wi::scene::Scene::EntitySerializeFlags::RECURSIVE);
    }

    archive->SetReadModeAndResetPos(true);
    auto root = wiscene.Entity_Serialize(*archive, seri, wi::ecs::INVALID_ENTITY,
        wi::scene::Scene::EntitySerializeFlags::RECURSIVE | wi::scene::Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);

    return root;
}

void Scene::Library_Update(float dt){
    // Directly load instance if not set to stream
    for(int i = 0; i < instances.GetCount(); ++i)
    {
        auto& instance = instances[i];
        auto instance_entity = instances.GetEntity(i);
        
        if(!streams.Contains(instance_entity) && (instance.scene == nullptr))
        {
            instance.scene = this;
            instance.instance_id = instance_entity;
            instance.Init();
        }
    }
    // Directly load scripts if not set to stream
    for(int i = 0; i < scriptobjects.GetCount(); ++i){
        auto& scriptobject = scriptobjects[i];
        auto scriptobject_entity = scriptobjects.GetEntity(i);
        
        if(!streams.Contains(scriptobject_entity) && (!scriptobject.initialized))
        {
            scriptobject.Init();
        }
    }
    for(int i = 0; i < streams.GetCount(); ++i)
    {
        auto& stream = streams[i];
        auto stream_entity = streams.GetEntity(i);

        if(stream.stream_zone.intersects(stream_boundary))
        {
            auto instance = instances.GetComponent(stream_entity);
            if(instance != nullptr){
                // Init instances first if the data is not loaded
                if(instance->scene == nullptr){
                    instance->scene = this;
                    instance->instance_id = stream_entity;
                    instance->Init();
                }

                // Once loading is done we then can transition this to the loaded data
                if(instance->collection_id != wi::ecs::INVALID_ENTITY){
                    stream.transition += dt*stream_transition_time;
                    stream.transition = std::min(stream.transition,1.f);

                    if(stream.transition > 0.f) // Preload script after it is done!
                    {
                        auto scriptobject = scriptobjects.GetComponent(stream_entity);
                        if(scriptobject != nullptr)
                        {
                            if(!scriptobject->initialized)
                                scriptobject->Init();
                        }
                    }
                }
            }
        }
        else
        {
            stream.transition -= dt*stream_transition_time;
            stream.transition = std::max(stream.transition,0.f);

            // Removes instance after transition finishes
            if(stream.transition == 0.f){
                auto instance = instances.GetComponent(stream_entity);
                if(instance != nullptr){
                    if(instance->scene != nullptr){
                        instance->Unload();
                        instance->scene = nullptr;
                    }
                }
                auto scriptobject = scriptobjects.GetComponent(stream_entity);
                if(scriptobject != nullptr) scriptobject->Unload();
            }
        }

        // If transition is ongoing, we're going to manipulate the transition of tracked objects!
        if(stream.transition > 0.f && stream.transition < 1.f){
            for(auto& object_it : stream.instance_original_transparency){
                auto objectComponent = wiscene.objects.GetComponent(object_it.first);
                objectComponent->color.w = object_it.second * stream.transition;
            }
        }
    }
}

void Scene::Update(float dt){
    Library_Update(dt);
}

void LiveUpdate::Update(float dt){
    
}