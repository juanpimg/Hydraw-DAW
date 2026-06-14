/* Hydraw DAW landing page — small interactions only. */

(() => {
  'use strict';

  /* ---------- spectrum bars ---------- */
  const spectrum = document.getElementById('spectrum');
  if (spectrum) {
    const BARS = 96;
    const frag = document.createDocumentFragment();
    const colors = ['#5599ff', '#66ddff', '#44bb66', '#eebb44', '#ee4444'];
    for (let i = 0; i < BARS; i++) {
      const s = document.createElement('span');
      const t = i / (BARS - 1);
      const colorIdx = Math.min(colors.length - 1, Math.floor(t * colors.length));
      s.style.setProperty('--bar-color', colors[colorIdx]);
      s.style.setProperty('--h', `${20 + Math.random() * 70}%`);
      s.style.setProperty('--d', `${0.6 + Math.random() * 1.2}s`);
      s.style.setProperty('--dl', `${Math.random() * -2}s`);
      frag.appendChild(s);
    }
    spectrum.appendChild(frag);
  }

  /* ---------- oscilloscope path ---------- */
  const scopePath = document.getElementById('scopePath');
  if (scopePath) {
    let raf = 0;
    const W = 200, H = 80, MID = 40;
    let phase = 0;
    const draw = () => {
      phase += 0.06;
      let d = `M0,${MID}`;
      for (let x = 0; x <= W; x += 2) {
        const t = x / W;
        const env = Math.sin(t * Math.PI) ** 1.4;
        const wob = Math.sin(t * 12 + phase) * 0.4
                  + Math.sin(t * 31 + phase * 1.7) * 0.25
                  + Math.sin(t * 67 + phase * 0.6) * 0.15
                  + (Math.random() - 0.5) * 0.4;
        const y = MID + wob * env * (MID * 0.85);
        d += ` L${x.toFixed(1)},${y.toFixed(1)}`;
      }
      scopePath.setAttribute('d', d);
      raf = requestAnimationFrame(draw);
    };
    draw();
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) cancelAnimationFrame(raf);
      else draw();
    });
  }

  /* ---------- copy to clipboard ---------- */
  document.querySelectorAll('.copy').forEach(btn => {
    btn.addEventListener('click', async () => {
      const txt = btn.getAttribute('data-copy') || '';
      try {
        await navigator.clipboard.writeText(txt);
      } catch {
        const ta = document.createElement('textarea');
        ta.value = txt; document.body.appendChild(ta);
        ta.select(); document.execCommand('copy'); ta.remove();
      }
      const orig = btn.textContent;
      btn.textContent = 'copied';
      btn.classList.add('copied');
      setTimeout(() => {
        btn.textContent = orig;
        btn.classList.remove('copied');
      }, 1400);
    });
  });

  /* ---------- scroll reveal ---------- */
  const reveals = document.querySelectorAll(
    '.section-head, .feature, .contrib, .install-steps li, .tl, .arch-wrap, .mockup, .credits, .arch-note, .dropcap'
  );
  reveals.forEach(el => el.classList.add('reveal'));

  if ('IntersectionObserver' in window) {
    const io = new IntersectionObserver((entries) => {
      entries.forEach(e => {
        if (e.isIntersecting) {
          e.target.classList.add('in');
          io.unobserve(e.target);
        }
      });
    }, { threshold: 0.12, rootMargin: '0px 0px -40px 0px' });
    reveals.forEach(el => io.observe(el));
  } else {
    reveals.forEach(el => el.classList.add('in'));
  }

  /* ---------- topbar contrast on scroll ---------- */
  const topbar = document.querySelector('.topbar');
  if (topbar) {
    const onScroll = () => {
      topbar.style.boxShadow = window.scrollY > 8
        ? '0 1px 0 var(--line) inset, 0 8px 24px -16px rgba(0,0,0,0.6)'
        : 'none';
    };
    onScroll();
    window.addEventListener('scroll', onScroll, { passive: true });
  }
})();
