include("script/campaign/libcampaign.js");

const mis_Labels = {
	startPos: {x: 13, y: 52},
	lz: {x: 10, y: 51, x2: 12, y2: 53},
	trPlace: {x: 11, y: 52},
	trExit: {x: 39, y: 126}
};

function eventStartLevel()
{
	camSetupTransporter(mis_Labels.trPlace.x, mis_Labels.trPlace.y, mis_Labels.trExit.x, mis_Labels.trExit.y);
	centreView(mis_Labels.startPos.x, mis_Labels.startPos.y);
	setNoGoArea(mis_Labels.lz.x, mis_Labels.lz.y, mis_Labels.lz.x2, mis_Labels.lz.y2, CAM_HUMAN_PLAYER);
	camSetMissionTimer(camChangeOnDiff(camHoursToSeconds(1)));
	camPlayVideos({video: "SB1_3_UPDATE", type: CAMP_MSG});
	camSetStandardWinLossConditions(CAM_VICTORY_PRE_OFFWORLD, cam_levels.alpha5.offWorld);
}
