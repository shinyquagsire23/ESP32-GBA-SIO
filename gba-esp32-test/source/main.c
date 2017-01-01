
#include <gba_console.h>
#include <gba_video.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_input.h>
#include <gba_sio.h>
#include <stdio.h>
#include <stdlib.h>

#include <xcomms.h>

int main ( void ) 
{
	void *store = malloc(0x10000);
	irqInit();
	irqEnable(IRQ_VBLANK);
	int i = 0;
	int rec = 0;
	
	consoleDemoInit();
	REG_IE=REG_IE|0x0080;	 	//serial interrupt enable
	xcomms_init();
	
	iprintf ("Waiting for link...\n");
	while(1)
	{
	    u32 rec = xcomms_exchange(0xDEADB00F);
	    
	    if(rec == 0xDEADB00F)
	    {
	        iprintf("\x1b[2J");
	        continue;
	    }
	    
	    iprintf("%.4s", &rec); //Just print everything sent
	}
}


