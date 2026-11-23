#pragma once

#include <d3dx9core.h>

class GameMenuManager // Never disposed
{
public:
	GameMenuManager();

	void			Render();

private:
	bool			Enabled;
	bool			EditingMode;
	UInt8			Pages[4];
	UInt8			SelectedPage[4];
	UInt8			Rows[4];
	UInt8			SelectedRow[4];
	UInt8			SelectedColumn;
	char			SettingValue[20];
	char			EditingValue[20];
	char			SelectedItem[40];
	char			SelectedDefinition[40];
	char			SelectedSection[40];
	char			SelectedSetting[40];
	char			InfoText[256];
	float			SelectedValue;
	UInt32			SelectedSectionKey;
	ID3DXFont*		FontSelected;
	ID3DXFont*		FontNormal;
	ID3DXFont*		FontStatus;
	RECT			Rect;
	RECT			RectShadow;
	RECT			RectInfo;
	RECT			RectInfoShadow;
	static const int kFrameRateWindow = 10;	///< Frames averaged per FPS overlay update

	int				FrameRate;			///< Last averaged frame rate shown by the FPS overlay
	int				FrameCounter;		///< Frames counted in the current FPS window
	double			FrameWindowStartMs;	///< FrameRateManager::GetPerformance() at the window start; < 0 restarts the window

};