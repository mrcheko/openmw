#include "actors.hpp"

#include <OgreSceneNode.h>
#include <OgreSceneManager.h>

#include "../mwworld/ptr.hpp"
#include "../mwworld/class.hpp"

#include "../mwrender/renderingmanager.hpp"

#include "animation.hpp"
#include "activatoranimation.hpp"
#include "creatureanimation.hpp"
#include "npcanimation.hpp"

#include "renderconst.hpp"


namespace MWRender
{
using namespace Ogre;

Actors::~Actors()
{
    PtrAnimationMap::iterator it = mAllActors.begin();
    for(;it != mAllActors.end();++it)
    {
        delete it->second;
        it->second = NULL;
    }
}

void Actors::setRootNode(Ogre::SceneNode* root)
{ mRootNode = root; }

void Actors::insertBegin(const MWWorld::Ptr &ptr)
{
    Ogre::SceneNode* cellnode;
    CellSceneNodeMap::const_iterator celliter = mCellSceneNodesLoad.find(ptr.getCell());
    if(celliter != mCellSceneNodesLoad.end())
        cellnode = celliter->second;
    else
    {
        //Create the scenenode and put it in the map
        //cellnode = mRootNode->createChildSceneNode(); mRootNode->addChild(cellnode);
        cellnode = new SceneNode(mRootNode->getCreator());
        mCellSceneNodesLoad[ptr.getCell()] = cellnode;
    }

    Ogre::SceneNode* insert = cellnode->createChildSceneNode();
    const float *f = ptr.getRefData().getPosition().pos;
    insert->setPosition(f[0], f[1], f[2]);
    insert->setScale(ptr.getCellRef().getScale(), ptr.getCellRef().getScale(), ptr.getCellRef().getScale());

    // Convert MW rotation to a quaternion:
    f = ptr.getCellRef().getPosition().rot;

    // For rendering purposes, actors should only rotate around the Z axis.
    // X rotation is used for camera rotation (for the player) and for
    // ranged magic / ranged weapon aiming.
    Ogre::Quaternion zr(Ogre::Radian(-f[2]), Ogre::Vector3::UNIT_Z);

    insert->setOrientation(zr);
    ptr.getRefData().setBaseNode(insert);
}

void Actors::insertNPC(const MWWorld::Ptr& ptr)
{
    insertBegin(ptr);
    NpcAnimation* anim = new NpcAnimation(ptr, ptr.getRefData().getBaseNode(), RV_Actors);
    mAllActorsLoad[ptr] = anim;
}
void Actors::insertCreature (const MWWorld::Ptr& ptr, bool weaponsShields)
{
    insertBegin(ptr);
    Animation* anim = NULL;
    if (weaponsShields)
        anim = new CreatureWeaponAnimation(ptr);
    else
        anim = new CreatureAnimation(ptr);
    mAllActorsLoad[ptr] = anim;
}
void Actors::initActors(std::list<MWWorld::Ptr>& new_objects)
{
    for (PtrAnimationMap::iterator iter = mAllActorsLoad.begin(); iter != mAllActorsLoad.end(); ++iter)
    {
        delete mAllActors[iter->first];
        mAllActors[iter->first] = iter->second;
        mRendering->addWaterRippleEmitter (iter->first);
        new_objects.push_back(iter->first);
    }

    mAllActorsLoad.clear();

    for (CellSceneNodeMap::iterator iter = mCellSceneNodesLoad.begin(); iter != mCellSceneNodesLoad.end(); ++iter)
    {
        CellSceneNodeMap::iterator cellnode = mCellSceneNodes.find(iter->first);

        if (cellnode == mCellSceneNodes.end())
        {
            mCellSceneNodes[iter->first] = iter->second;
        }
        else
        {
            Ogre::SceneNode* cellnodeLoad = iter->second;
            Ogre::Node::ChildNodeIterator children = cellnodeLoad->getChildIterator();
            while(children.hasMoreElements())
            {
                Ogre::Node *child = children.getNext();
                cellnodeLoad->removeChild(child);
                cellnode->second->addChild(child);
            }
        }
    }

    mCellSceneNodesLoad.clear();
}
void Actors::insertActivator (const MWWorld::Ptr& ptr)
{
    insertBegin(ptr);
    ActivatorAnimation* anim = new ActivatorAnimation(ptr);
    delete mAllActorsLoad[ptr];
    mAllActorsLoad[ptr] = anim;
}

bool Actors::deleteObject (const MWWorld::Ptr& ptr)
{
    if (mAllActors.find(ptr) == mAllActors.end())
        return false;

    mRendering->removeWaterRippleEmitter (ptr);

    delete mAllActors[ptr];
    mAllActors.erase(ptr);

    if(Ogre::SceneNode *base=ptr.getRefData().getBaseNode())
    {
        Ogre::SceneNode *parent = base->getParentSceneNode();
        CellSceneNodeMap::const_iterator iter(mCellSceneNodes.begin());
        for(;iter != mCellSceneNodes.end();++iter)
        {
            if(iter->second == parent)
            {
                base->removeAndDestroyAllChildren();
                mRend.getScene()->destroySceneNode (base);
                ptr.getRefData().setBaseNode (0);
                return true;
            }
        }

        return false;
    }

    return true;
}

void Actors::removeCell(MWWorld::CellStore* store)
{
    for(PtrAnimationMap::iterator iter = mAllActors.begin();iter != mAllActors.end();)
    {
        if(iter->first.getCell() == store)
        {
            mRendering->removeWaterRippleEmitter (iter->first);
            delete iter->second;
            mAllActors.erase(iter++);
        }
        else
            ++iter;
    }
    CellSceneNodeMap::iterator celliter = mCellSceneNodes.find(store);
    if(celliter != mCellSceneNodes.end())
    {
        Ogre::SceneNode *base = celliter->second;
        base->removeAndDestroyAllChildren();
        mRend.getScene()->destroySceneNode(base);

        mCellSceneNodes.erase(celliter);
    }
}

void Actors::update (Ogre::Camera* camera)
{
    for(PtrAnimationMap::iterator iter = mAllActors.begin();iter != mAllActors.end(); ++iter)
    {
        iter->second->preRender(camera);
    }
}

Animation* Actors::getAnimation(const MWWorld::Ptr &ptr)
{
    PtrAnimationMap::const_iterator iter = mAllActors.find(ptr);
    if(iter != mAllActors.end())
        return iter->second;
    return NULL;
}

void Actors::updateObjectCell(const MWWorld::Ptr &old, const MWWorld::Ptr &cur)
{
    Ogre::SceneNode *node;
    MWWorld::CellStore *newCell = cur.getCell();

    CellSceneNodeMap::const_iterator celliter = mCellSceneNodes.find(newCell);
    if(celliter != mCellSceneNodes.end())
        node = celliter->second;
    else
    {
        node = mRootNode->createChildSceneNode();
        mCellSceneNodes[newCell] = node;
    }
    node->addChild(cur.getRefData().getBaseNode());

    PtrAnimationMap::iterator iter = mAllActors.find(old);
    if(iter != mAllActors.end())
    {
        Animation *anim = iter->second;
        mAllActors.erase(iter);
        anim->updatePtr(cur);
        mAllActors[cur] = anim;
    }

    mRendering->updateWaterRippleEmitterPtr (old, cur);
}

}
