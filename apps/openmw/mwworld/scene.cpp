#include "scene.hpp"

#include <OgreSceneNode.h>

#include <components/nif/niffile.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp" /// FIXME
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "physicssystem.hpp"
#include "player.hpp"
#include "localscripts.hpp"
#include "esmstore.hpp"
#include "class.hpp"
#include "cellfunctors.hpp"
#include "cellstore.hpp"

namespace
{
    void updateObjectLocalRotation (const MWWorld::Ptr& ptr, MWWorld::PhysicsSystem& physics)
    {
        if (ptr.getRefData().getBaseNode() != NULL)
        {
            Ogre::Quaternion worldRotQuat(Ogre::Radian(ptr.getRefData().getPosition().rot[2]), Ogre::Vector3::NEGATIVE_UNIT_Z);
            if (!ptr.getClass().isActor())
                worldRotQuat = Ogre::Quaternion(Ogre::Radian(ptr.getRefData().getPosition().rot[0]), Ogre::Vector3::NEGATIVE_UNIT_X)*
                        Ogre::Quaternion(Ogre::Radian(ptr.getRefData().getPosition().rot[1]), Ogre::Vector3::NEGATIVE_UNIT_Y)* worldRotQuat;

            float x = ptr.getRefData().getLocalRotation().rot[0];
            float y = ptr.getRefData().getLocalRotation().rot[1];
            float z = ptr.getRefData().getLocalRotation().rot[2];

            Ogre::Quaternion rot(Ogre::Radian(z), Ogre::Vector3::NEGATIVE_UNIT_Z);
            if (!ptr.getClass().isActor())
                rot = Ogre::Quaternion(Ogre::Radian(x), Ogre::Vector3::NEGATIVE_UNIT_X)*
                Ogre::Quaternion(Ogre::Radian(y), Ogre::Vector3::NEGATIVE_UNIT_Y)*rot;

            ptr.getRefData().getBaseNode()->setOrientation(worldRotQuat*rot);
            physics.rotateObject(ptr);
        }
    }

    void adjustObject(std::list<MWWorld::Ptr>& new_refs, MWWorld::PhysicsSystem& physics)
    {
        for (std::list<MWWorld::Ptr>::iterator ptr = new_refs.begin(); ptr != new_refs.end(); ++ptr)
        {
			try
			{
				if (ptr->getRefData().getCount() && ptr->getRefData().isEnabled())
				{
					ptr->getClass().insertObject (*ptr, physics);

					updateObjectLocalRotation(*ptr, physics);
					if (ptr->getCellRef().getScale() != 1.0f) MWBase::Environment::get().getWorld()->scaleObject (*ptr, ptr->getCellRef().getScale());
					ptr->getClass().adjustPosition (*ptr, false);
				}
			}
			catch (const std::exception& e)
			{
				std::string error ("error during rendering: ");
				std::cerr << error + e.what() << std::endl;
			}
        }
    }

    struct InsertFunctor
    {
        MWWorld::CellStore& mCell;
        bool mRescale;
        Loading::Listener& mLoadingListener;
        MWWorld::PhysicsSystem& mPhysics;
        MWRender::RenderingManager& mRendering;

        InsertFunctor (MWWorld::CellStore& cell, bool rescale, Loading::Listener& loadingListener,
            MWWorld::PhysicsSystem& physics, MWRender::RenderingManager& rendering);

        bool operator() (const MWWorld::Ptr& ptr);
    };

    InsertFunctor::InsertFunctor (MWWorld::CellStore& cell, bool rescale,
        Loading::Listener& loadingListener, MWWorld::PhysicsSystem& physics,
        MWRender::RenderingManager& rendering)
    : mCell (cell), mRescale (rescale), mLoadingListener (loadingListener),
      mPhysics (physics), mRendering (rendering)
    {}

    bool InsertFunctor::operator() (const MWWorld::Ptr& ptr)
    {
        if (mRescale)
        {
            if (ptr.getCellRef().getScale()<0.5)
                ptr.getCellRef().setScale(0.5);
            else if (ptr.getCellRef().getScale()>2)
                ptr.getCellRef().setScale(2);
        }

        if (ptr.getRefData().getCount() && ptr.getRefData().isEnabled())
        {
            try
            {
                mRendering.addObject (ptr);
            }
            catch (const std::exception& e)
            {
                std::string error ("error during rendering: ");
                std::cerr << error + e.what() << std::endl;
            }
        }

        //mLoadingListener.increaseProgress (1);

        return true;
    }
}


namespace MWWorld
{

    void Scene::updateObjectLocalRotation (const Ptr& ptr)
    {
        ::updateObjectLocalRotation(ptr, *mPhysics);
    }

    void Scene::updateObjectRotation (const Ptr& ptr)
    {
        if(ptr.getRefData().getBaseNode() != 0)
        {
            mRendering.rotateObject(ptr);
            mPhysics->rotateObject(ptr);
        }
    }

    void Scene::loadCellsThread()
    {
        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();

        // go through all cells to load
        for (CellStoreCollection::iterator it = mCellsLoading.begin(); it != mCellsLoading.end();)
        {
            insertCell(*(*it), true, loadingListener);
            mCellsLoad.insert(*it);
            it = mCellsLoading.erase(it);
        }
    }

    void Scene::update (float duration, const ESM::Position& position, bool paused)
    {
        if (mNeedMapUpdate)
        {
            // Note: exterior cell maps must be updated, even if they were visited before, because the set of surrounding cells might be different
            // (and objects in a different cell can "bleed" into another cells map if they cross the border)
            for (CellStoreCollection::iterator active = mActiveCells.begin(); active!=mActiveCells.end(); ++active)
                mRendering.requestMap(*active);
            mNeedMapUpdate = false;
        }

        mRendering.update (duration, paused);

        // load cells in background

        CellStore *curCell = getCurrentCell();

        if (!curCell->getCell()->isExterior()) return;

        // update cell loading threads states
		// there can be only 1 background cell loading thread (to ensure thread-safeness)
        if (isCellLoading()) return;

        // load on <= distLoad units to cell border
        // unload on > distUnload units of cell border
        static const float distLoad = 1000, distUnload = 1500;

        int curX = curCell->getCell()->getGridX();
        int curY = curCell->getCell()->getGridY();

        std::cerr<<"thread current:"<<curX<<","<<curY<<std::endl;

        int cellOffsetX = 0, cellOffsetY = 0;

        // check if we closer to left or right border
        if(position.pos[0] - curX * ESM::Land::REAL_SIZE <= distLoad) cellOffsetX = -2;
        else if((curX+1) * ESM::Land::REAL_SIZE - position.pos[0] <= distLoad) cellOffsetX = 2;

        // check if we closer to top or bottom border
        if(position.pos[1] - curY * ESM::Land::REAL_SIZE <= distLoad) cellOffsetY = -2;
        else if((curY+1) * ESM::Land::REAL_SIZE - position.pos[1] <= distLoad) cellOffsetY = 2;

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);

        std::string loadingExteriorText = "#{sLoadingMessage3}";
        loadingListener->setLabel(loadingExteriorText);

        loadingListener->setProgressRange(1000);

        // check if need to load 3 cells only for 1 side: top bottom left right
        if((cellOffsetX != 0 && cellOffsetY == 0) || (cellOffsetX == 0 && cellOffsetY != 0))
        {
            /* Example for cellOffsetY < 0 (|x| - cells to load)
                _ _ _
               |x|x|x|
               |_|_|_|
               |_|_|_|
               |_|_|_|*/
            CellStore *cell;
            for (int i = -1; i <= -1; i++)
            {
                int cellX = (cellOffsetX != 0 ? curX + cellOffsetX : curX + i);
                int cellY = (cellOffsetY != 0 ? curY + cellOffsetY : curY + i);

                cell = MWBase::Environment::get().getWorld()->getExterior(cellX, cellY);
                if (!isCellActive(*cell) && !isCellLoaded(*cell))
                {
                    mCellsLoading.insert(cell);
                    std::cerr<<"thread load 3:"<<cellX<<","<<cellY<<std::endl;
					return;
                }
            }
        }

        // moving at diagonal cell
        if(cellOffsetX != 0 && cellOffsetY != 0)
        {
            /* Example for cellOffsetX < 0 && cellOffsetY <0 (|x| - cells to load)
              _ _ _
             |x|x|x|_
             |x|_|_|_|
             |x|_|_|_|
               |_|_|_|*/
            CellStore *cell;

            int cellX, cellY = curY + cellOffsetY;
            for (int i = 0; i < 5; i++)
            {
                if(i < 3) cellX = curX + (cellOffsetX < 0 ? -i : i); // first 3 cells
                else cellY = curY + (cellOffsetY < 0 ? i-2 : -i+2); // last 2

                cell = MWBase::Environment::get().getWorld()->getExterior(cellX, cellY);
                if (!isCellActive(*cell) && !isCellLoaded(*cell))
                {
                    mCellsLoading.insert(cell);
                    std::cerr<<"thread load 5:"<<cellX<<","<<cellY<<std::endl;
					return;
                }
            }
        }

        if (!mCellsLoading.empty()) mCellsLoadThread = new boost::thread(boost::bind(&Scene::loadCellsThread, this));
    }

    void Scene::unloadCell (CellStoreCollection::iterator iter)
    {
        std::cout << "Unloading cell\n";
        ListAndResetHandles functor;

        (*iter)->forEach<ListAndResetHandles>(functor);
        {
            // silence annoying g++ warning
            for (std::vector<Ogre::SceneNode*>::const_iterator iter2 (functor.mHandles.begin());
                iter2!=functor.mHandles.end(); ++iter2)
            {
                Ogre::SceneNode* node = *iter2;
                mPhysics->removeObject (node->getName());
            }
        }

        if ((*iter)->getCell()->isExterior())
        {
            ESM::Land* land =
                MWBase::Environment::get().getWorld()->getStore().get<ESM::Land>().search(
                    (*iter)->getCell()->getGridX(),
                    (*iter)->getCell()->getGridY()
                );
            if (land)
                mPhysics->removeHeightField ((*iter)->getCell()->getGridX(), (*iter)->getCell()->getGridY());
        }

        mRendering.removeCell(*iter);

        MWBase::Environment::get().getWorld()->getLocalScripts().clearCell (*iter);

        MWBase::Environment::get().getMechanicsManager()->drop (*iter);

        MWBase::Environment::get().getSoundManager()->stopSound (*iter);
        mActiveCells.erase(*iter);
    }

    void Scene::loadCell(CellStore *cell, Loading::Listener* loadingListener, bool loadRefs)
    {
        std::pair<CellStoreCollection::iterator, bool> result = mActiveCells.insert(cell);

        if(result.second)
        {
            float verts = ESM::Land::LAND_SIZE;
            float worldsize = ESM::Land::REAL_SIZE;

            // Load terrain physics first...
            if (cell->getCell()->isExterior())
            {
                ESM::Land* land =
                    MWBase::Environment::get().getWorld()->getStore().get<ESM::Land>().search(
                        cell->getCell()->getGridX(),
                        cell->getCell()->getGridY()
                    );
                if (land) {
                    mPhysics->addHeightField (
                        land->mLandData->mHeights,
                        cell->getCell()->getGridX(),
                        cell->getCell()->getGridY(),
                        0,
                        worldsize / (verts-1),
                        verts)
                    ;
                }
            }

            cell->respawn();

            // ... then references. This is important for adjustPosition to work correctly.
            /// \todo rescale depending on the state of a new GMST
            if (loadRefs) 
            {
                // only load objects/actors data from hard drive here
                insertCell (*cell, true, loadingListener);

                // initialize objects for rendering
                std::list<MWWorld::Ptr> new_refs;
                mRendering.initObjects(new_refs);
                adjustObject(new_refs, *mPhysics);

                cellAdded(cell);
            }
        }
    }

    void Scene::cellAdded(CellStore *cell)
    {
        mRendering.cellAdded (cell);
        mRendering.configureAmbient(*cell);

        // register local scripts
        MWBase::Environment::get().getWorld()->getLocalScripts().addCell (cell);
    }

    void Scene::playerCellChange(CellStore *cell, const ESM::Position& pos, bool adjustPlayerPos)
    {
        MWBase::World *world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr old = world->getPlayerPtr();
        world->getPlayer().setCell(cell);

        MWWorld::Ptr player = world->getPlayerPtr();
        mRendering.updatePlayerPtr(player);

        if (adjustPlayerPos) {
            world->moveObject(player, pos.pos[0], pos.pos[1], pos.pos[2]);

            float x = Ogre::Radian(pos.rot[0]).valueDegrees();
            float y = Ogre::Radian(pos.rot[1]).valueDegrees();
            float z = Ogre::Radian(pos.rot[2]).valueDegrees();
            world->rotateObject(player, x, y, z);

            player.getClass().adjustPosition(player, true);
        }

        MWBase::MechanicsManager *mechMgr =
            MWBase::Environment::get().getMechanicsManager();

        mechMgr->updateCell(old, player);
        mechMgr->watchActor(player);

        mRendering.updateTerrain();

        // Delay the map update until scripts have been given a chance to run.
        // If we don't do this, objects that should be disabled will still appear on the map.
        mNeedMapUpdate = true;

        MWBase::Environment::get().getWindowManager()->changeCell(mCurrentCell);
    }

    void Scene::changeToVoid()
    {
        CellStoreCollection::iterator active = mActiveCells.begin();
        while (active!=mActiveCells.end())
            unloadCell (active++);
        assert(mActiveCells.empty());
        mCurrentCell = NULL;
    }

    void Scene::changeCell (int X, int Y, const ESM::Position& position, bool adjustPlayerPos)
    {
        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);

        mRendering.enableTerrain(true);

        std::string loadingExteriorText = "#{sLoadingMessage3}";
        loadingListener->setLabel(loadingExteriorText);

        // wait until background cells loading complete
        if (isCellLoading()) 
        {
            mCellsLoadThread->join();
            delete mCellsLoadThread;
            mCellsLoadThread = NULL;
        }

        for (CellStoreCollection::iterator it = mCellsLoad.begin(); it != mCellsLoad.end(); ++it)
        {
            loadCell(*it, loadingListener, false); // refs were loaded in background
        }

        std::list<MWWorld::Ptr> new_refs;
        mRendering.initObjects(new_refs);
        adjustObject(new_refs, *mPhysics);

        for (CellStoreCollection::iterator it = mCellsLoad.begin(); it != mCellsLoad.end(); ++it)
        {
            cellAdded(*it);
        }

        mCellsLoad.clear();

        CellStoreCollection::iterator active = mActiveCells.begin();

        active = mActiveCells.begin();
        while (active!=mActiveCells.end())
        {
            if ((*active)->getCell()->isExterior())
            {
                if (std::abs (X-(*active)->getCell()->getGridX())<=1 &&
                    std::abs (Y-(*active)->getCell()->getGridY())<=1)
                {
                    // keep cells within the new 3x3 grid
                    ++active;
                    continue;
                }
            }
            unloadCell (active++);
        }

        int refsToLoad = 0;
        // get the number of refs to load
        for (int x=X-1; x<=X+1; ++x)
            for (int y=Y-1; y<=Y+1; ++y)
            {
                CellStoreCollection::iterator iter = mActiveCells.begin();

                while (iter!=mActiveCells.end())
                {
                    assert ((*iter)->getCell()->isExterior());

                    if (x==(*iter)->getCell()->getGridX() &&
                        y==(*iter)->getCell()->getGridY())
                        break;

                    ++iter;
                }

                if (iter==mActiveCells.end())
                    refsToLoad += MWBase::Environment::get().getWorld()->getExterior(x, y)->count();
            }

        loadingListener->setProgressRange(refsToLoad);

        // Load cells
        for (int x=X-1; x<=X+1; ++x)
            for (int y=Y-1; y<=Y+1; ++y)
            {
                CellStoreCollection::iterator iter = mActiveCells.begin();

                while (iter!=mActiveCells.end())
                {
                    assert ((*iter)->getCell()->isExterior());

                    if (x==(*iter)->getCell()->getGridX() &&
                        y==(*iter)->getCell()->getGridY())
                        break;

                    ++iter;
                }

                if (iter==mActiveCells.end())
                {
                    CellStore *cell = MWBase::Environment::get().getWorld()->getExterior(x, y);

                    loadCell (cell, loadingListener, true);
                }
            }

        // find current cell
        CellStoreCollection::iterator iter = mActiveCells.begin();

        while (iter!=mActiveCells.end())
        {
            assert ((*iter)->getCell()->isExterior());

            if (X==(*iter)->getCell()->getGridX() &&
                Y==(*iter)->getCell()->getGridY())
                break;

            ++iter;
        }

        assert (iter!=mActiveCells.end());

        mCurrentCell = *iter;

        // adjust player
        playerCellChange (mCurrentCell, position, adjustPlayerPos);

        // Sky system
        MWBase::Environment::get().getWorld()->adjustSky();

        mCellChanged = true;
    }

    //We need the ogre renderer and a scene node.
    Scene::Scene (MWRender::RenderingManager& rendering, PhysicsSystem *physics)
    : mCurrentCell (0), mCellChanged (false), mPhysics(physics), mRendering(rendering), mNeedMapUpdate(false), mCellsLoadThread(NULL)
    {
    }

    Scene::~Scene()
    {
    }

    bool Scene::hasCellChanged() const
    {
        return mCellChanged;
    }

    const Scene::CellStoreCollection& Scene::getActiveCells() const
    {
        return mActiveCells;
    }

    void Scene::changeToInteriorCell (const std::string& cellName, const ESM::Position& position)
    {
        CellStore *cell = MWBase::Environment::get().getWorld()->getInterior(cellName);
        bool loadcell = (mCurrentCell == NULL);
        if(!loadcell)
            loadcell = *mCurrentCell != *cell;

        MWBase::Environment::get().getWindowManager()->fadeScreenOut(0.5);

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        std::string loadingInteriorText = "#{sLoadingMessage2}";
        loadingListener->setLabel(loadingInteriorText);
        Loading::ScopedLoad load(loadingListener);

        mRendering.enableTerrain(false);

        if(!loadcell)
        {
            MWBase::World *world = MWBase::Environment::get().getWorld();
            world->moveObject(world->getPlayerPtr(), position.pos[0], position.pos[1], position.pos[2]);

            float x = Ogre::Radian(position.rot[0]).valueDegrees();
            float y = Ogre::Radian(position.rot[1]).valueDegrees();
            float z = Ogre::Radian(position.rot[2]).valueDegrees();
            world->rotateObject(world->getPlayerPtr(), x, y, z);

            world->getPlayerPtr().getClass().adjustPosition(world->getPlayerPtr(), true);
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);
            return;
        }

        std::cout << "Changing to interior\n";

        // remove active
        CellStoreCollection::iterator active = mActiveCells.begin();

        // unload
        int current = 0;
        active = mActiveCells.begin();
        while (active!=mActiveCells.end())
        {
            unloadCell (active++);
            ++current;
        }

        int refsToLoad = cell->count();
        loadingListener->setProgressRange(refsToLoad);

        // Load cell.
        std::cout << "cellName: " << cell->getCell()->mName << std::endl;

        //Loading Interior loading text

        loadCell (cell, loadingListener, true);

        mCurrentCell = cell;

        // adjust fog
        mRendering.configureFog(*mCurrentCell);

        // adjust player
        playerCellChange (mCurrentCell, position);

        // Sky system
        MWBase::Environment::get().getWorld()->adjustSky();

        mCellChanged = true;
        MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);
    }

    void Scene::changeToExteriorCell (const ESM::Position& position)
    {
        int x = 0;
        int y = 0;

        MWBase::Environment::get().getWorld()->positionToIndex (position.pos[0], position.pos[1], x, y);

        changeCell (x, y, position, true);
    }

    CellStore* Scene::getCurrentCell ()
    {
        return mCurrentCell;
    }

    void Scene::markCellAsUnchanged()
    {
        mCellChanged = false;
    }

    void Scene::insertCell (CellStore &cell, bool rescale, Loading::Listener* loadingListener)
    {
        InsertFunctor functor (cell, rescale, *loadingListener, *mPhysics, mRendering);
        cell.forEach (functor);
    }

    void Scene::addObjectToScene (const Ptr& ptr)
    {
        mRendering.addObject(ptr);
        ptr.getClass().insertObject(ptr, *mPhysics);
        MWBase::Environment::get().getWorld()->rotateObject(ptr, 0, 0, 0, true);
        MWBase::Environment::get().getWorld()->scaleObject(ptr, ptr.getCellRef().getScale());
    }

    void Scene::removeObjectFromScene (const Ptr& ptr)
    {
        MWBase::Environment::get().getMechanicsManager()->remove (ptr);
        MWBase::Environment::get().getSoundManager()->stopSound3D (ptr);
        mPhysics->removeObject (ptr.getRefData().getHandle());
        mRendering.removeObject (ptr);
    }

    bool Scene::isCellActive(const CellStore &cell)
    {
        CellStoreCollection::iterator active = mActiveCells.begin();
        while (active != mActiveCells.end()) {
            if (**active == cell) {
                return true;
            }
            ++active;
        }
        return false;
    }

	bool Scene::isCellLoaded(const CellStore &cell)
	{
		CellStoreCollection::iterator active = mCellsLoad.begin();
        while (active != mCellsLoad.end()) {
            if (**active == cell) {
                return true;
            }
            ++active;
        }
        return false;
	}

    bool Scene::isCellLoading()
    {
        return mCellsLoadThread != NULL;
    }

    bool Scene::checkCellsLoaded()
    {
        // if thread has finished working then delete it from list
        if (mCellsLoadThread != NULL) 
        {
            if(!mCellsLoadThread->timed_join(boost::posix_time::milliseconds(0))) 
            {
                delete mCellsLoadThread;
                mCellsLoadThread = NULL;
                return true;
            }
            else return false;
        }

        return true;
    }

    Ptr Scene::searchPtrViaHandle (const std::string& handle)
    {
        for (CellStoreCollection::const_iterator iter (mActiveCells.begin());
            iter!=mActiveCells.end(); ++iter)
            if (Ptr ptr = (*iter)->searchViaHandle (handle))
                return ptr;

        return Ptr();
    }

    Ptr Scene::searchPtrViaActorId (int actorId)
    {
        for (CellStoreCollection::const_iterator iter (mActiveCells.begin());
            iter!=mActiveCells.end(); ++iter)
            if (Ptr ptr = (*iter)->searchViaActorId (actorId))
                return ptr;

        return Ptr();
    }
}
