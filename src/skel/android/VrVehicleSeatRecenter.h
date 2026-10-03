#pragma once

// Track the actual occupant separately from the renderer's basis-mode edge.
// Menus/tracking loss postpone a pending seat change; neither rearms a
// seat already centered. No stored pointer is dereferenced.
struct VrVehicleSeatRecenter
{
	const void *player = nullptr;
	const void *vehicle = nullptr;
	int seat = -1;
	bool centered = false;

	void Reset()
	{
		player = vehicle = nullptr;
		seat = -1;
		centered = false;
	}

	bool Update(const void *nextPlayer, const void *nextVehicle, int nextSeat,
	            bool seatReady, bool postPhysics, bool trackingReady)
	{
		if(nextPlayer == nullptr || nextVehicle == nullptr || nextSeat < 0){
			Reset();
			return false;
		}
		if(player != nextPlayer || vehicle != nextVehicle || seat != nextSeat){
			player = nextPlayer;
			vehicle = nextVehicle;
			seat = nextSeat;
			centered = false;
		}
		if(centered || !seatReady || !postPhysics || !trackingReady)
			return false;
		centered = true;
		return true;
	}
};
