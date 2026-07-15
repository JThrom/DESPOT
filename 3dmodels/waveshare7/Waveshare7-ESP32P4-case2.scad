$fn=50;

Outer_Width=103;
Outer_Length=168;
Outer_Depth=25;

Inner_Width=100;
Inner_Length=165;
Inner_Depth=22;
Corner_Depth=15;
Corner_Bore=4;
Corner_Boss=10;
Corner_Boss_Height=5.5;
Countersink_Height=4;

module corner(pos=[x, y, z]){
    translate([pos.x,pos.y,pos.z]){
            cube([Corner_Boss,Corner_Boss, Corner_Boss_Height], center=true);
    }
}
module cornerHole(pos=[x, y, z]){
    translate([pos.x,pos.y,pos.z]){
            cylinder(h=Inner_Depth*2, d=Corner_Bore, center=true);

}}

module cornerSink(pos=[x, y, z]){
    translate([pos.x,pos.y,pos.z]){
            #cylinder(h=Countersink_Height, d=Corner_Bore*2, center=true);

}}

module usbCHole (pos=[x,y,z]){
    translate([pos.x, pos.y, pos.z]){
    #cube([11, 30, 3], center=true);
    }
}
    
    
module usbAHole (pos=[x,y,z]){
    translate([pos.x, pos.y, pos.z]){
    #cube([15, 30, 6], center=true);
    }
    }

module corners(){
                    corner(pos=[Inner_Width/2-Corner_Boss/2, Inner_Length/2-Corner_Boss/2, -5.25]);
    
                corner(pos=[(Inner_Width/2-Corner_Boss/2)*-1, Inner_Length/2-Corner_Boss/2, -5.25]);
            
            corner(pos=[Inner_Width/2-Corner_Boss/2, (Inner_Length/2-Corner_Boss/2)*-1, -5.25]);
    
                corner(pos=[(Inner_Width/2-Corner_Boss/2)*-1, (Inner_Length/2-Corner_Boss/2)*-1, -5.25]);
    }
    
    module cornerHoles(){
                    cornerHole(pos=[Inner_Width/2-Corner_Boss/2, Inner_Length/2-Corner_Boss/2, (Inner_Depth/2-Corner_Boss_Height/2)*-1]);
        
                cornerHole(pos=[(Inner_Width/2-Corner_Boss/2)*-1, Inner_Length/2-Corner_Boss/2, (Inner_Depth/2-Corner_Boss_Height/2)*-1]);
            
            cornerHole(pos=[Inner_Width/2-Corner_Boss/2, (Inner_Length/2-Corner_Boss/2)*-1, (Inner_Depth/2-Corner_Boss_Height/2)*-1]);
        
                cornerHole(pos=[(Inner_Width/2-Corner_Boss/2)*-1, (Inner_Length/2-Corner_Boss/2)*-1, (Inner_Depth/2-Corner_Boss_Height/2)*-1]);
    }

    module cornerCountersinks(){
                    cornerSink(pos=[Inner_Width/2-Corner_Boss/2, Inner_Length/2-Corner_Boss/2, (Outer_Depth/2-Countersink_Height/2)*-1]);
        
                cornerSink(pos=[(Inner_Width/2-Corner_Boss/2)*-1, Inner_Length/2-Corner_Boss/2, (Outer_Depth/2-Countersink_Height/2)*-1]);
            
            cornerSink(pos=[Inner_Width/2-Corner_Boss/2, (Inner_Length/2-Corner_Boss/2)*-1, (Outer_Depth/2-Countersink_Height/2)*-1]);
        
                cornerSink(pos=[(Inner_Width/2-Corner_Boss/2)*-1, (Inner_Length/2-Corner_Boss/2)*-1, (Outer_Depth/2-Countersink_Height/2)*-1]);
    }


difference(){
    group(){
        //corner bosses
        corners();
        
        difference(){
            //outer box
        group(){
                cube([Outer_Width, Outer_Length, Outer_Depth], center=true);
            }
            //inner box
        group(){
            translate([0,0,(Outer_Depth-Inner_Depth)]){
                cube([Inner_Width, Inner_Length, Inner_Depth], center=true);
            }
            //box usb holes
            usbCHole(pos=[33.5,Outer_Length/2, 3]);
            usbCHole(pos=[-20,Outer_Length/2, 2]);
            usbCHole(pos=[-35,Outer_Length/2, 2]);
            usbAHole(pos=[-0.5,Outer_Length/2, 0.5]);
                
        }
}
        
        
        }
    group(){
        //corner through holes
        cornerCountersinks();
        cornerHoles();
        }
    }




