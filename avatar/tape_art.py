import sys, os, random, math
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
D = os.environ["FONTS"]

def worn_tape(w, h, text, size, seed, NOTE="#07"):
    rnd=random.Random(seed); S=3; W,H=w*S,h*S
    base=Image.new('RGBA',(W,H),(240,237,228,255)); d=ImageDraw.Draw(base)
    for x in range(0,W,4):   # crepe crinkles
        shade=rnd.choice(((230,227,218),(246,243,235),(235,232,223)))
        d.line([(x,0),(x+rnd.randint(-2,2)*S,H)],fill=shade+(255,),width=2)
    light=Image.new('RGBA',(W,H),(0,0,0,0)); ld=ImageDraw.Draw(light)
    for y in range(H):
        ld.line([(0,y),(W,y)],fill=(255,255,255,int(18*math.sin(y/H*math.pi))))
    base.alpha_composite(light)
    wear=Image.new('RGBA',(W,H),(0,0,0,0)); wd=ImageDraw.Draw(wear)
    for y in list(range(0,10*S))+list(range(H-12*S,H)):   # grime along both edges
        edge=min(y,H-1-y)/(12*S)
        for x in range(0,W,6):
            if rnd.random()<0.35*(1-edge):
                wd.point((x+rnd.randint(0,5),y),fill=(90,84,72,rnd.randint(40,110)))
    for _ in range(4):   # fingerprint smudges
        cx,cy=rnd.randint(int(W*0.15),int(W*0.85)),rnd.randint(int(H*0.2),int(H*0.8))
        rx,ry=rnd.randint(18,30)*S,rnd.randint(12,18)*S
        wd.ellipse([cx-rx,cy-ry,cx+rx,cy+ry],fill=(110,104,92,26))
        for k in range(3,rx,4*S):
            wd.ellipse([cx-k,cy-k*ry//rx,cx+k,cy+k*ry//rx],outline=(100,94,82,16),width=S)
    for _ in range(3):   # scuffs
        x0,y0=rnd.randint(0,W),rnd.randint(0,H)
        wd.line([(x0,y0),(x0+rnd.randint(20,60)*S,y0+rnd.randint(-6,6)*S)],fill=(120,114,100,60),width=2*S)
    base.alpha_composite(wear.filter(ImageFilter.GaussianBlur(S*1.2)))
    # worn marker
    font=ImageFont.truetype(os.path.join(D,"PermanentMarker.ttf"),size*S)
    t=Image.new('RGBA',(W,H),(0,0,0,0)); td=ImageDraw.Draw(t)
    td.text((W//2,H//2+S*3),text,fill=(20,20,24,235),anchor="mm",font=font)
    rub=Image.new('L',(W,H),255); rd=ImageDraw.Draw(rub)
    for _ in range(140):
        x,y=rnd.randint(0,W),rnd.randint(0,H)
        rd.ellipse([x-2*S,y-S,x+2*S,y+S],fill=rnd.randint(90,200))
    t.putalpha(ImageChops.multiply(t.split()[3],rub.filter(ImageFilter.GaussianBlur(S))))
    base.alpha_composite(t.rotate(-2,resample=Image.BICUBIC,center=(W//2,H//2)))
    # ballpoint asset number
    pen=ImageFont.truetype(os.path.join(D,"PermanentMarker.ttf"),int(size*0.32)*S)
    p=Image.new('RGBA',(W,H),(0,0,0,0)); pd=ImageDraw.Draw(p)
    pd.text((int(W*0.78),int(H*0.70)),NOTE,fill=(40,60,130,170),anchor="mm",font=pen)
    pd.line([(int(W*0.72),int(H*0.85)),(int(W*0.84),int(H*0.83))],fill=(40,60,130,120),width=S)
    base.alpha_composite(p)
    # nicks along the long edges, slightly translucent tape
    top=[(x, rnd.choice((0,0,0,S,2*S,4*S))) for x in range(0,W+1,6*S)]
    bot=[(x, H-1-rnd.choice((0,0,0,S,2*S,5*S))) for x in range(W,-1,-6*S)]
    mask=Image.new('L',(W,H),0); ImageDraw.Draw(mask).polygon(top+bot,fill=240)
    base.putalpha(mask)
    return base.resize((w,h),Image.LANCZOS)
