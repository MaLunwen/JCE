"""Render UI labels with Pillow's bundled default font or an explicit font input.

The default Aileron subset is No Rights Reserved. Original restricted display
font software and its local reference glyphs stay outside published resources.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import math
import json
import argparse
import hashlib
import PIL

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[2]
OUT = PROJECT/'resources/ui'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path, help='Explicit authoring font; caller supplies its redistribution terms')
    args = parser.parse_args()
    if args.font and not args.font.is_file():
        parser.error('font input does not exist')
    prototype = ImageFont.truetype(str(args.font), 20) if args.font else ImageFont.load_default(size=20)
    font_bytes = args.font.read_bytes() if args.font else prototype.font_bytes
    provenance = {'schema':1, 'provider':'explicit-font' if args.font else 'Pillow bundled default',
                  'pillow_version':PIL.__version__, 'family':prototype.getname(),
                  'font_sha256':hashlib.sha256(font_bytes).hexdigest(), 'font_bytes':len(font_bytes),
                  'license':'caller supplied' if args.font else 'No Rights Reserved',
                  'source':'caller supplied' if args.font else 'https://dotcolon.net/fonts/aileron/',
                  'scope':'Rasterized UI images only; no font software is redistributed',
                  'original_typography':'Deliberately replaced by redistributable default typography; original captures remain reference evidence'}
    OUT.mkdir(parents=True,exist_ok=True)
    metrics = {}
    def label(name,text,size,color=(255,255,255,255)):
        import math
        scale=4
        font = ImageFont.truetype(str(args.font),size*scale) if args.font else ImageFont.load_default(size=size*scale)
        bounds=font.getbbox(text); advance=font.getlength(text)/scale
        height=size*10/7 if size<36 else size*1.1
        width=max(1,math.ceil(max(advance,bounds[2]/scale)))
        image=Image.new('RGBA',(width*scale,math.ceil(height)*scale))
        top=7.333 if size==22 else (4 if size==14 else (height-(bounds[3]-bounds[1])/scale)/2)
        ImageDraw.Draw(image).text((-bounds[0],top*scale-bounds[1]),text,font=font,fill=color,stroke_width=2 if name.startswith('cost-red-') else 0,stroke_fill=color)
        link='Fellicht.nl' if name=='credit-line' else ('pracalic.ugu.pl' if name=='pracalic' else None)
        if link:
            start=text.index(link);left=font.getlength(text[:start])-bounds[0];right=left+font.getlength(link)
            # Tint only glyph pixels within the mixed-colour hyperlink span.
            import numpy as np
            pixels=np.array(image);pixels[:,int(left):math.ceil(right),:3]=(66,139,202)
            image=Image.fromarray(pixels)
        image=image.resize((width,math.ceil(height)),Image.Resampling.LANCZOS)
        image.save(OUT/(name+'.png')); metrics[name]=[advance,height,width,math.ceil(height)]
    for digit in range(10):
        label('digit-'+str(digit),str(digit),22,(0,0,0,255))
        label('cost-'+str(digit),str(digit),22)
        label('cost-red-'+str(digit),str(digit),22,(255,0,0,255))
    for name,text,size,color in [('back','Back to menu',14,(0,0,0,255)),('wave','Next [w]ave',14,(255,255,255,255)),
        ('play','Play',14,(255,255,255,255)),('level','Play level 1',14,(255,255,255,255)),
        ('credits','Credits',14,(0,0,0,255)),('title','Tower Defense',36,(255,255,255,255)),
        ('credit-line','Created by Mathieu de Ruiter (Fellicht.nl)',22,(255,255,255,255)),
        ('level-line','Update your towers, buy new weapons, etc here...',22,(255,255,255,255)),
        ('close','close',14,(0,0,0,255)),('credits-title','Credits',18,(34,34,34,255)),
        ('programmer','Programmer',22,(34,34,34,255)),
        ('designer','Designer',22,(34,34,34,255)),
        ('thanks','Thanks to',22,(34,34,34,255)),
        ('mathieu','Mathieu de Ruiter',22,(34,34,34,255)),
        ('timothy','Timothy',22,(34,34,34,255)),
        ('pracalic','Pracalic (pracalic.ugu.pl)',22,(34,34,34,255)),
        ('cross','×',22,(66,139,202,255)),
        ('slash','/',22,(0,0,0,255))]: label(name,text,size,color)
    scale=4
    frame=Image.new('RGBA',(77*scale,42*scale),(238,238,238,255)); draw=ImageDraw.Draw(frame); b=5*scale; w,h=frame.size
    draw.polygon([(0,0),(w,0),(w-b,b),(b,b),(b,h-b),(0,h)],fill='white')
    draw.polygon([(w,0),(w,h),(w-b,h-b),(w-b,b)],fill='#b9b9b9')
    draw.polygon([(0,h),(w,h),(w-b,h-b),(b,h-b)],fill='#8f8f8f')
    frame.resize((77,42),Image.Resampling.LANCZOS).save(OUT/'frame.png')
    for name,fill,border in [('button-blue','#428bca','#357ebd'),('button-white','#ffffff','#cccccc')]:
        image=Image.new('RGBA',(132*scale,34*scale)); draw=ImageDraw.Draw(image)
        draw.rounded_rectangle((0,0,image.width-1,image.height-1),radius=4*scale,fill=fill,outline=border,width=scale)
        image.resize((132,34),Image.Resampling.LANCZOS).save(OUT/(name+'.png'))
    image=Image.new('RGBA',(77*scale,math.ceil(33.3333*scale)));draw=ImageDraw.Draw(image)
    draw.rounded_rectangle((0,0,image.width-1,image.height-1),radius=4*scale,fill='#428bca',outline='#357ebd',width=round(2*scale/3))
    image.resize((77,34),Image.Resampling.LANCZOS).save(OUT/'button-blue-small.png')
    image=Image.new('RGBA',(600*scale,297*scale));draw=ImageDraw.Draw(image)
    draw.rounded_rectangle((0,0,image.width-1,image.height-1),radius=6*scale,fill='#ffffff',outline='#999999',width=scale)
    for y in [56,232]:draw.line((0,y*scale,image.width,y*scale),fill='#e5e5e5',width=scale)
    image.resize((600,297),Image.Resampling.LANCZOS).save(OUT/'modal.png')
    header=['/* Generated by author_ui.py; text canvas sizes are display pixels. */']
    header.append('static void td_ui_metric(const char *name, float *w, float *h) {')
    header.append('    *w=0; *h=0;')
    for name,v in metrics.items(): header.append(f'    if(!strcmp(name,"{name}")) {{ *w={v[2]:.6f}f; *h={v[3]:.6f}f; return; }}')
    header.append('}')
    (PROJECT/'src/ui_metrics.h').write_text('\n'.join(header)+'\n',encoding='utf-8',newline='\n')
    (PROJECT/'build/ui-metrics.json').write_text(json.dumps(metrics,indent=2),encoding='utf-8',newline='\n')
    (PROJECT/'resources/licenses/ui-font-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n',encoding='utf-8',newline='\n')
    print('UI ART',len(metrics),'labels/glyphs; no bundled font software')

if __name__ == '__main__': main()
