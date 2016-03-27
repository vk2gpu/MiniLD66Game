#include "GaGameComponent.h"

#include "GaCameraComponent.h"
#include "GaUnitComponent.h"

#include "System/Debug/DsImGui.h"

#include "System/Scene/ScnComponentProcessor.h"
#include "System/Scene/ScnCore.h"
#include "System/Scene/ScnEntity.h"

#include "System/Scene/Rendering/ScnCanvasComponent.h"
#include "System/Scene/Rendering/ScnDebugRenderComponent.h"
#include "System/Scene/Rendering/ScnViewComponent.h"

#include "System/Os/OsCore.h"


REFLECTION_DEFINE_DERIVED( GaGameComponent );

void GaGameComponent::StaticRegisterClass()
{
	ReField* Fields[] = 
	{
		new ReField( "TickHz_", &GaGameComponent::TickHz_, bcRFF_IMPORTER ),
		new ReField( "TestEntity_", &GaGameComponent::TestEntity_, bcRFF_IMPORTER | bcRFF_SHALLOW_COPY ),
		
		new ReField( "TickRate_", &GaGameComponent::TickRate_, bcRFF_TRANSIENT ),
		new ReField( "TickAccumulator_", &GaGameComponent::TickAccumulator_, bcRFF_TRANSIENT ),
	};

	using namespace std::placeholders;
	ReRegisterClass< GaGameComponent, Super >( Fields )
		.addAttribute( new ScnComponentProcessor( 
			{
				ScnComponentProcessFuncEntry::Update< GaGameComponent >()
			} ) );
}


GaGameComponent::GaGameComponent()
{
	InputState_.fill( InputState::IDLE );
}


GaGameComponent::~GaGameComponent()
{
}


void GaGameComponent::onAttach( ScnEntityWeakRef Parent )
{
	Super::onAttach( Parent );

	Camera_ = Parent->getComponentAnyParentByType< ScnEntity >( "CameraEntity" )->getComponentByType< GaCameraComponent >();
	BcAssert( Camera_ );

	Material_  = Parent->getComponentAnyParentByType< ScnMaterialComponent >();
	BcAssert( Material_  );

	Canvas_  = Parent->getComponentAnyParentByType< ScnCanvasComponent >();
	BcAssert( Canvas_ );

	TickRate_ = GaReal( 1.0f ) / GaReal( TickHz_ );
	TickAccumulator_ = TickRate_;
	
	// Input events.
	OsCore::pImpl()->subscribe( osEVT_INPUT_MOUSEDOWN, this, 
		[ this ]( EvtID ID, const EvtBaseEvent& InEvent )
		{
			auto Event = InEvent.get< OsEventInputMouse >();
			auto& InputState = InputState_[ Event.ButtonCode_ ];
			auto& BeginDragMouseEvent = BeginDragMouseEvent_[ Event.ButtonCode_ ];
			switch( InputState )
			{
			case InputState::IDLE:
				InputState = InputState::DOWN;
				BeginDragMouseEvent = Event;
				break;
			case InputState::DOWN:
				InputState = InputState::DOWN;
				BeginDragMouseEvent = Event;
				break;
			case InputState::DRAGGING:
				InputState = InputState::DRAGGING;
				onCancelDrag( Event ); // Shouldn't have been dragging, cancel it.
				BeginDragMouseEvent = Event;
				onBeginDrag( Event );
				break;
			}

			LastMouseEvent_ = Event;
			return evtRET_PASS;
		} );

	OsCore::pImpl()->subscribe( osEVT_INPUT_MOUSEUP, this,
		[ this ]( EvtID ID, const EvtBaseEvent& InEvent )
		{
			auto Event = InEvent.get< OsEventInputMouse >();
			auto& InputState = InputState_[ Event.ButtonCode_ ];
			switch( InputState )
			{
			case InputState::IDLE:
				InputState = InputState::DOWN;
				break;
			case InputState::DOWN:
				InputState = InputState::IDLE;
				onClick( Event );
				break;
			case InputState::DRAGGING:
				InputState = InputState::IDLE;
				onEndDrag( Event );
				break;
			}
			LastMouseEvent_ = Event;
			return evtRET_PASS;
		} );

	OsCore::pImpl()->subscribe( osEVT_INPUT_MOUSEMOVE, this,
		[ this ]( EvtID ID, const EvtBaseEvent& InEvent )
		{
			for( size_t Idx = 0; Idx < InputState_.size(); ++Idx )
			{
				auto Event = InEvent.get< OsEventInputMouse >();
				auto& InputState = InputState_[ Idx ];
				auto& BeginDragMouseEvent = BeginDragMouseEvent_[ Idx ];
				switch( InputState )
				{
				case InputState::IDLE:
					InputState = InputState::IDLE;
					break;
				case InputState::DOWN:
					{
						InputState = InputState::DOWN;

						// If mouse moves enough, switch to dragging.
						MaVec2d A( BeginDragMouseEvent.MouseX_, BeginDragMouseEvent.MouseY_ );
						MaVec2d B( Event.MouseX_, Event.MouseY_ );

						// Wide range for drag for non-mouse people.
						if( ( A - B ).magnitude() > 16.0f )
						{
							InputState = InputState::DRAGGING;
							onBeginDrag( BeginDragMouseEvent );
						}
					}				
					break;
				case InputState::DRAGGING:
					InputState = InputState::DRAGGING;
					Event.ButtonCode_ = Idx;
					onUpdateDrag( Event );
					break;
				}
			}

			LastMouseEvent_ = InEvent.get< OsEventInputMouse >();
			return evtRET_PASS;
		} );

}


void GaGameComponent::onDetach( ScnEntityWeakRef Parent )
{
	OsCore::pImpl()->unsubscribeAll( this );

	Super::onDetach( Parent );
}

void GaGameComponent::onBeginDrag( OsEventInputMouse Event )
{
	SelectionBoxEnable_ = BcTrue;
	SelectionBoxA_ = MaVec2d( Event.MouseX_, Event.MouseY_ );
	SelectionBoxB_ = MaVec2d( Event.MouseX_, Event.MouseY_ );
}


void GaGameComponent::onUpdateDrag( OsEventInputMouse Event )
{
	if( Event.ButtonCode_ == 0 )
	{
		SelectionBoxB_ = MaVec2d( Event.MouseX_, Event.MouseY_ );
	}
}


void GaGameComponent::onEndDrag( OsEventInputMouse Event )
{
	if( Event.ButtonCode_ == 0 )
	{
		SelectionBoxB_ = MaVec2d( Event.MouseX_, Event.MouseY_ );
		SelectionBoxEnable_ = BcFalse;

		SelectedUnitIDs_.clear();

		// Do selection. Use centre points of units, rather than frustum (do that later maybe?)
		// Check for unit.
		for( auto* Unit : Units_ )
		{
			auto ScreenPos = Camera_->getScreenPosition( Unit->getParentEntity()->getWorldPosition() );
			auto A = MaVec2d( 
				std::min( SelectionBoxA_.x(), SelectionBoxB_.x() ),
				std::min( SelectionBoxA_.y(), SelectionBoxB_.y() ) );
			auto B = MaVec2d( 
				std::max( SelectionBoxA_.x(), SelectionBoxB_.x() ),
				std::max( SelectionBoxA_.y(), SelectionBoxB_.y() ) );		

			if( ScreenPos.x() > A.x() && ScreenPos.y() > A.y() && 
				ScreenPos.x() < B.x() && ScreenPos.y() < B.y() )
			{
				SelectedUnitIDs_.push_back( Unit->getID() );
			}
		}
	}

	// Do click anyway.
	if( Event.ButtonCode_ == 1 )
	{
		onClick( Event );
	}
}


void GaGameComponent::onCancelDrag( OsEventInputMouse Event )
{
	if( Event.ButtonCode_ == 0 )
	{
		SelectionBoxEnable_ = BcFalse;
	}
}


void GaGameComponent::onClick( OsEventInputMouse Event )
{
	if( Event.ButtonCode_ == 0 )
	{
		SelectedUnitIDs_.clear();

		// Check for unit.
		for( auto* Unit : Units_ )
		{
			MaVec3d NearPos, FarPos;
			Camera_->getWorldPosition( MaVec2d( Event.MouseX_, Event.MouseY_ ), NearPos, FarPos );
			auto SpatialComponent = Unit->getComponentByType< ScnSpatialComponent >();
			if( SpatialComponent != nullptr )
			{
				auto AABB = SpatialComponent->getAABB();

				MaVec3d Intersection;
				if( AABB.lineIntersect( NearPos, FarPos, &Intersection, nullptr ) )
				{	
					SelectedUnitIDs_.push_back( Unit->getID() );
				}
			}
		}
	}

	if( Event.ButtonCode_ == 1 )
	{
		// If we have selection, move to position on grid.
		MaVec3d NearPos, FarPos;
		Camera_->getWorldPosition( MaVec2d( Event.MouseX_, Event.MouseY_ ), NearPos, FarPos );

		MaPlane GroundPlane;
		GroundPlane.fromPointNormal( MaVec3d( 0.0f, 0.0f, 0.0f ), MaVec3d( 0.0f, 1.0f, 0.0f ) );
		auto Dir = ( FarPos - NearPos ).normal();
		BcF32 Distance = 0.0f;
		MaVec3d Intersection;
		if( GroundPlane.lineIntersection( NearPos, FarPos, Distance, Intersection ) )
		{
			// Find midpoint.
			GaVec3d Midpoint( 0.0f, 0.0f, 0.0f );
			GaReal NoofUnits = 0;
			for( auto UnitID : SelectedUnitIDs_ )
			{
				auto* Unit = getUnit( UnitID );
				if( Unit )
				{
					Midpoint += Unit->getState().Position_;
					NoofUnits += 1.0f;
				}
			}

			if( NoofUnits > 0.0f )
			{
				Midpoint /= NoofUnits;

				// TODO: Position them in formation.
			
				// Send all units to offsets around this position.
				for( auto UnitID : SelectedUnitIDs_ )
				{
					auto* Unit = getUnit( UnitID );
					if( Unit )
					{
						auto Centre = GaVec3d( Intersection.x(), Intersection.y(), Intersection.z() );
						auto MovePosition = ( Unit->getState().Position_ - Midpoint ) + Centre;

						Unit->commandMove( MovePosition );
					}
				}
			}
		}

	}
}


void GaGameComponent::update( BcF32 Tick )
{
	static bool InterpolatedRender = true;

#if !PSY_PRODUCTION
	if ( ImGui::Begin( "Engine Debug" ) )
	{
		if( ImGui::TreeNode( "GaGameComponent" ) )
		{
			ImGui::Checkbox( "Enable interpolated render", &InterpolatedRender );

			if( ImGui::Button( "Spawn test entity" ) )
			{
				spawnUnit( TestEntity_, TeamID_, GaVec3d( 0.0f, 0.0f, 0.0f ) );
			}

			if( Units_.size() > 0 )
			{
				static float TargetPosition[3] = { 0.0f, 0.0f, 0.0f };
				ImGui::InputFloat3( "Target Position", TargetPosition );
				if( ImGui::Button( "Test Target Position" ) )
				{
					Units_.back()->commandMove( GaVec3d( TargetPosition[0], TargetPosition[1], TargetPosition[2] ) );
				}			
			}
			ImGui::TreePop();
		}
	}
	ImGui::End();
#endif

	// Simulation.
	TickAccumulator_ += Tick;
	while( TickAccumulator_ >= TickRate_ )
	{
		TickAccumulator_ -= TickRate_;

		// Add all pending units.
		for( auto* Unit : PendingRegisterUnits_ )
		{
			Units_.push_back( Unit );
		}
		PendingRegisterUnits_.clear();

		// Update unit state.
		for( auto* Unit : Units_ )
		{
			Unit->updateState();
		}

		// Update units.
		for( auto* Unit : Units_ )
		{
			Unit->update( TickRate_ );
		}

		// Remove all pending units.
		for( auto* Unit : PendingDeregisterUnits_ )
		{
			auto FoundIt = std::find( Units_.begin(), Units_.end(), Unit );
			if( FoundIt != Units_.end() )
			{
				Units_.erase( FoundIt );
				DestroyedUnits_.push_back( Unit );
				Unit->destroyUnit();
			}
		}
		PendingDeregisterUnits_.clear();
	}

#if !PSY_PRODUCTION
	auto DebugRender = ScnDebugRenderComponent::pImpl();
	BcAssert( DebugRender );

	DebugRender->drawGrid( MaVec3d( 0.0f, 0.0f, 0.0f ), MaVec3d( 1000.0f, 0.0f, 1000.0f ), 1.0f, 10.0f );
#endif

	// Rendering.
	const BcF32 Alpha = InterpolatedRender ? TickAccumulator_ / TickRate_ : 0;
	for( auto* Unit : Units_ )
	{
		GaUnitState State = Unit->getInterpolatedState( Alpha );

		MaVec3d Position( State.Position_.x(), State.Position_.y(), State.Position_.z() );

		Unit->getParentEntity()->setLocalPosition( Position );
	}

	// Draw selection (debug).
	for( auto UnitID : SelectedUnitIDs_ )
	{
		auto* Unit = getUnit( UnitID );
		if( Unit )
		{
			GaUnitState State = Unit->getInterpolatedState( Alpha );
			MaVec3d Position( State.Position_.x(), State.Position_.y(), State.Position_.z() );

			// Debug rendering.
#if !PSY_PRODUCTION
			MaVec3d Velocity( State.Velocity_.x(), State.Velocity_.y(), State.Velocity_.z() );
			DebugRender->drawLine( Position, Position + Velocity, RsColour::GREEN, 0 );
			DebugRender->drawEllipsoid( Position, MaVec3d( 0.51f, 0.51f, 0.51f ), RsColour::GREEN, 0 );
#endif
		}
	}

	// 2D HUD stuff.
	{
		if( SelectionBoxEnable_ )
		{
			Canvas_->setMaterialComponent( Material_ );
			Canvas_->drawBox( SelectionBoxA_, SelectionBoxB_, RsColour( 0.0f, 1.0f, 0.0f, 0.1f ) );
			Canvas_->drawLineBox( SelectionBoxA_, SelectionBoxB_, RsColour( 0.0f, 1.0f, 0.0f, 1.0f ) );
		}
	}

	// Mouse debug.
	{
#if !PSY_PRODUCTION
		MaVec2d MousePosition( LastMouseEvent_.MouseX_, LastMouseEvent_.MouseY_ );
		MaVec3d NearPos, FarPos;
		Camera_->getWorldPosition( MousePosition, NearPos, FarPos );

		MaPlane GroundPlane;
		GroundPlane.fromPointNormal( MaVec3d( 0.0f, 0.0f, 0.0f ), MaVec3d( 0.0f, 1.0f, 0.0f ) );
		auto Dir = ( FarPos - NearPos ).normal();
		BcF32 Distance = 0.0f;
		MaVec3d Intersection;
		if( GroundPlane.lineIntersection( NearPos, FarPos, Distance, Intersection ) )
		{
			DebugRender->drawEllipsoid( Intersection, MaVec3d( 1.0f, 1.0f, 1.0f ), RsColour::WHITE, 0 );
		}
#endif
	}

	drawMinimap();

}


void GaGameComponent::drawMinimap()
{
	OsClient* Client = OsCore::pImpl()->getClient( 0 );
	MaVec2d ClientSize( Client->getWidth(), Client->getHeight() );
	MaVec2d Size( 100.0f, 100.0f );
	MaMat4d Transform;
	Transform.translation( ClientSize - MaVec2d( 100.0f, 100.0f ) );
	Canvas_->pushMatrix( Transform );
	Canvas_->drawBox( -Size, Size, RsColour( 0.05f, 0.05f, 0.05f, 1.0f ), 0 );
	
	for( auto* Unit : Units_ )
	{
		auto Position = Unit->getParentEntity()->getWorldPosition();
		if( Position.x() > -99.0f && Position.x() < 99.0f &&
			Position.z() > -99.0f && Position.z() < 99.0f )
		{
			Canvas_->drawBox( Position.xz() - MaVec2d( 1.0f, 1.0f ), Position.xz() + MaVec2d( 1.0f, 1.0f ), RsColour( 1.0f, 1.0f, 1.0f, 1.0f ), 0 );
		}
	}

	Canvas_->drawLineBox( -Size, Size, RsColour( 0.0f, 0.0f, 0.0f, 1.0f ), 0 );
	Canvas_->popMatrix();
}


class GaUnitComponent* GaGameComponent::getUnit( BcU32 UnitID )
{
	// TODO: Lookup table.
	for( auto* Unit : Units_ )
	{
		if( Unit->getID() == UnitID )
		{
			return Unit;
		}
	}
	return nullptr;
}


void GaGameComponent::spawnUnit( class ScnEntity* BaseEntity, BcU32 TeamID, GaVec3d Position )
{
	MaVec4d SpawnPosition;

	auto* Entity = ScnCore::pImpl()->spawnEntity( ScnEntitySpawnParams( BcName::INVALID, BaseEntity, MaMat4d(), getParentEntity() ) );
	BcAssert( Entity );
	auto* Unit = Entity->getComponentByType< GaUnitComponent >();
	BcAssert( Unit );
	Unit->setupUnit( CurrentUnitID_++, TeamID, Position );
	PendingRegisterUnits_.push_back( Unit );
}


void GaGameComponent::destroyUnit( BcU32 UnitID )
{
	// TODO: Lookup table.
	for( auto* Unit : Units_ )
	{
		if( Unit->getID() == UnitID )
		{
			PendingDeregisterUnits_.push_back( Unit );
			break;
		}
	}
}
