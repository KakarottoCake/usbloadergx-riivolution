/****************************************************************************
 * Copyright (C) 2010
 * by Dimok
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any
 * damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you
 * must not claim that you wrote the original software. If you use
 * this software in a product, an acknowledgment in the product
 * documentation would be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and
 * must not be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source
 * distribution.
 ***************************************************************************/
#include <unistd.h>
#include "PromptWindow.hpp"
#include "menu/menus.h"
#include "settings/CSettings.h"
#include "themes/CTheme.h"

//! Where the message may be drawn, in pixels down from the top of the dialogue
//! box. The title's glyph row is centred on y=55 in a 26px font, so it ends
//! about y=68; the first button image is 44px tall and sits 55px up from the
//! bottom of a 320px box, so it starts at y=221. Everything between is the
//! message's, and nothing outside it is.
//!
//! This matters because GuiText centres a wrapped block on its own position
//! and grows out of it in BOTH directions - it does not clip. A message
//! longer than the band therefore rides up over the title and is drawn on top
//! of it, which is exactly what a long Riivolution warning did. Centring the
//! block in the band instead of 40px above the box's middle is what buys the
//! room; FitMessageText below is what keeps a message from spending more than
//! there is.
static const int PROMPT_MSG_TOP    = 72;
static const int PROMPT_MSG_BOTTOM = 219;
static const int PROMPT_MSG_CENTER = (PROMPT_MSG_TOP + PROMPT_MSG_BOTTOM) / 2;

PromptWindow::PromptWindow(const char *title, const char *msg)
	: GuiWindow(472, 320)
{
	SetAlignment(ALIGN_CENTER, ALIGN_MIDDLE);
	SetPosition(0, -10);

	btnOutline = Resources::GetImageData("button_dialogue_box.png");
	dialogBox = Resources::GetImageData("dialogue_box.png");

	width = dialogBox->GetWidth();
	height = dialogBox->GetHeight();

	trigA = new GuiTrigger;
	trigA->SetSimpleTrigger(-1, WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A, PAD_BUTTON_A);
	trigB = new GuiTrigger;
	trigB->SetButtonOnlyTrigger(-1, WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B, PAD_BUTTON_B);

	dialogBoxImg = new GuiImage(dialogBox);
	if(Settings.wsprompt)
		dialogBoxImg->SetWidescreen(Settings.widescreen);
	Append(dialogBoxImg);

	titleTxt = new GuiText(title, 26, thColor("r=0 g=0 b=0 a=255 - prompt windows text color"));
	titleTxt->SetAlignment(ALIGN_CENTER, ALIGN_TOP);
	titleTxt->SetPosition(0, 55);
	if(Settings.wsprompt && Settings.widescreen)
		titleTxt->SetMaxWidth(width-140, DOTTED);
	else
		titleTxt->SetMaxWidth(width-40, DOTTED);
	Append(titleTxt);

	msgTxt = new GuiText(msg, 22, thColor("r=0 g=0 b=0 a=255 - prompt windows text color"));
	msgTxt->SetAlignment(ALIGN_CENTER, ALIGN_MIDDLE);
	msgTxt->SetPosition(0, PROMPT_MSG_CENTER - (int)(height / 2));
	if(Settings.wsprompt && Settings.widescreen)
		msgTxt->SetMaxWidth(width-140, WRAP);
	else
		msgTxt->SetMaxWidth(width-40, WRAP);
	msgBaseSize = msgTxt->GetFontSize();
	Append(msgTxt);

	FitMessageText();

	SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_IN, 50);
}

PromptWindow::~PromptWindow()
{
	ResumeGui();

	SetEffect(EFFECT_SLIDE_TOP | EFFECT_SLIDE_OUT, 50);
	while(parentElement && this->GetEffect() > 0) usleep(100);

	HaltGui();
	if(parentElement)
		((GuiWindow *) parentElement)->Remove(this);
	parentElement = NULL;

	RemoveAll();

	delete btnOutline;
	delete dialogBox;

	delete trigA;
	delete trigB;

	delete dialogBoxImg;

	delete titleTxt;
	delete msgTxt;

	for(u32 i = 0; i < Button.size(); ++i)
	{
		delete ButtonTxt[i];
		delete ButtonImg[i];
		delete Button[i];
	}

	ResumeGui();
}

//! How many lines GuiText will actually wrap this message into at this font
//! size. This walks the string the same way GuiText::WrapText does, using the
//! same per-character widths, because guessing from the unwrapped width is
//! wrong in the direction that hurts: word wrapping leaves a ragged right
//! edge, so a guess undercounts, and an undercount is what puts text on top
//! of the title. The bound on linenum only stops a pathological string from
//! spinning here - the fit loop has already given up long before it.
static int CountWrappedLines(const wchar_t *text, int size, int maxWidth)
{
	int i = 0;
	int ch = 0;
	int linenum = 0;
	int lastSpace = -1;
	int currentWidth = 0;

	while (text[ch] && linenum < 64)
	{
		currentWidth += fontSystem->getCharWidth(text[ch], size, ch > 0 ? text[ch - 1] : 0x0000);

		if (currentWidth >= maxWidth)
		{
			if (lastSpace >= 0)
			{
				ch = lastSpace;
				lastSpace = -1;
			}
			currentWidth = 0;
			++linenum;
			i = -1;
		}
		if (text[ch] == ' ' && i >= 0)
			lastSpace = ch;

		++ch;
		++i;
	}

	return linenum + 1;
}

void PromptWindow::FitMessageText()
{
	const int band = PROMPT_MSG_BOTTOM - PROMPT_MSG_TOP;
	const int maxW = msgTxt->GetTextMaxWidth();
	const wchar_t *wmsg = msgTxt->GetText();

	//! GuiText's line pitch is the font size plus six, and the block is
	//! centred on the band's centre, so the whole block is lines * pitch tall
	//! and must not exceed the band. Take the largest size that fits; stop at
	//! 12, below which it is not worth reading on a television.
	int size = msgBaseSize;

	if (wmsg && *wmsg && maxW > 0)
	{
		for (; size > 12; --size)
		{
			if (CountWrappedLines(wmsg, size, maxW) * (size + 6) <= band)
				break;
		}
	}

	msgTxt->SetFontSize(size);

	//! Anything still over the band after shrinking is cut with an ellipsis
	//! rather than drawn over the title. A warning sitting on top of its own
	//! heading reads as neither.
	int cap = band / (size + 6);
	if (cap < 1)
		cap = 1;
	msgTxt->SetLinesToDraw(cap);
}

void PromptWindow::SetMessageText(const char *text)
{
	msgTxt->SetText(text);
	FitMessageText();
}

void PromptWindow::PositionButtons()
{
	if (Settings.wsprompt && Settings.widescreen)
	{
		switch(Button.size())
		{
			default:
				break;
			case 1:
				Button[0]->SetAlignment(ALIGN_CENTER, ALIGN_BOTTOM);
				Button[0]->SetPosition(0, -55);
				break;
			case 2:
				Button[0]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[0]->SetPosition(70, -55);
				Button[1]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[1]->SetPosition(-70, -55);
				break;
			case 3:
				Button[0]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[0]->SetPosition(70, -120);
				Button[1]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[1]->SetPosition(-70, -120);
				Button[2]->SetAlignment(ALIGN_CENTER, ALIGN_BOTTOM);
				Button[2]->SetPosition(0, -55);
				break;
			case 4:
				Button[0]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[0]->SetPosition(70, -120);
				Button[1]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[1]->SetPosition(-70, -120);
				Button[2]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[2]->SetPosition(70, -55);
				Button[3]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[3]->SetPosition(-70, -55);
				break;
		}
	}
	else
	{
		switch(Button.size())
		{
			default:
				break;
			case 1:
				Button[0]->SetAlignment(ALIGN_CENTER, ALIGN_BOTTOM);
				Button[0]->SetPosition(0, -55);
				break;
			case 2:
				Button[0]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[0]->SetPosition(50, -55);
				Button[1]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[1]->SetPosition(-50, -55);
				break;
			case 3:
				Button[0]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[0]->SetPosition(50, -120);
				Button[1]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[1]->SetPosition(-50, -120);
				Button[2]->SetAlignment(ALIGN_CENTER, ALIGN_BOTTOM);
				Button[2]->SetPosition(0, -55);
				break;
			case 4:
				Button[0]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[0]->SetPosition(50, -120);
				Button[1]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[1]->SetPosition(-50, -120);
				Button[2]->SetAlignment(ALIGN_LEFT, ALIGN_BOTTOM);
				Button[2]->SetPosition(50, -55);
				Button[3]->SetAlignment(ALIGN_RIGHT, ALIGN_BOTTOM);
				Button[3]->SetPosition(-50, -55);
				break;
		}
	}
}

void PromptWindow::AddButton(const char *text)
{
	int size = Button.size();
	if(size > 3)
		return;

	ButtonTxt.resize(size+1);
	ButtonImg.resize(size+1);
	Button.resize(size+1);

	ButtonTxt[size] = new GuiText(text, 20, thColor("r=0 g=0 b=0 a=255 - prompt windows button text color"));
	ButtonImg[size] = new GuiImage(btnOutline);
	if (Settings.wsprompt)
	{
		ButtonTxt[size]->SetWidescreen(Settings.widescreen);
		ButtonImg[size]->SetWidescreen(Settings.widescreen);
	}

	Button[size] = new GuiButton(ButtonImg[size], ButtonImg[size], 0, 3, 0, 0, trigA, btnSoundOver, btnSoundClick2, 1);
	Button[size]->SetLabel(ButtonTxt[size]);
	Button[size]->SetState(STATE_SELECTED);
	Button[size]->SetTrigger(1, trigB);
	Append(Button[size]);

	if(size > 0)
		Button[size-1]->SetTrigger(1, NULL);

	PositionButtons();
}

void PromptWindow::RemoveButton()
{
	Remove(Button[Button.size()-1]);

	delete ButtonTxt[ButtonTxt.size()-1];
	delete ButtonImg[ButtonImg.size()-1];
	delete Button[Button.size()-1];

	ButtonTxt.pop_back();
	ButtonImg.pop_back();
	Button.pop_back();

	if(ButtonImg.size() > 0)
		Button[ButtonImg.size()-1]->SetTrigger(1, trigB);

	PositionButtons();
}

void PromptWindow::RemoveButton(int i)
{
	if(i >= 0 && i < (int) Button.size())
		Remove(Button[i]);
}

int PromptWindow::GetChoice()
{
	for(u32 i = 0; i < Button.size(); ++i)
	{
		if(Button[i]->GetState() == STATE_CLICKED)
		{
			Button[i]->ResetState();
			return (i+1 != Button.size()) ? i+1 : 0;
		}
	}

	return -1;
}
