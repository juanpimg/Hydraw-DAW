(function () {
  'use strict';

  /* ─── spectrum bars (hero) ─── */
  var spectrum = document.getElementById('spectrum');
  if (spectrum && !spectrum.children.length) {
    var BARS = 96;
    var frag = document.createDocumentFragment();
    var colors = ['#5599ff', '#66ddff', '#44bb66', '#eebb44', '#ee5555'];
    for (var i = 0; i < BARS; i++) {
      var s = document.createElement('span');
      var t = i / (BARS - 1);
      var colorIdx = Math.min(colors.length - 1, Math.floor(t * colors.length));
      s.style.setProperty('--bar-color', colors[colorIdx]);
      s.style.setProperty('--h', (20 + Math.random() * 70) + '%');
      s.style.setProperty('--d', (0.6 + Math.random() * 1.2) + 's');
      s.style.setProperty('--dl', (Math.random() * -2) + 's');
      frag.appendChild(s);
    }
    spectrum.appendChild(frag);
  }

  /* ─── oscilloscope (hero) ─── */
  var scopePath = document.getElementById('scopePath');
  if (scopePath) {
    var raf = 0;
    var W = 200, MID = 32;
    var phase = 0;
    var draw = function () {
      phase += 0.06;
      var d = 'M0,' + MID;
      for (var x = 0; x <= W; x += 2) {
        var t = x / W;
        var env = Math.pow(Math.sin(t * Math.PI), 1.4);
        var wob = Math.sin(t * 12 + phase) * 0.4
                + Math.sin(t * 31 + phase * 1.7) * 0.25
                + Math.sin(t * 67 + phase * 0.6) * 0.15
                + (Math.random() - 0.5) * 0.4;
        var y = MID + wob * env * (MID * 0.85);
        d += ' L' + x.toFixed(1) + ',' + y.toFixed(1);
      }
      scopePath.setAttribute('d', d);
      raf = requestAnimationFrame(draw);
    };
    draw();
    document.addEventListener('visibilitychange', function () {
      if (document.hidden) cancelAnimationFrame(raf);
      else draw();
    });
  }

  /* ─── copy to clipboard ─── */
  document.querySelectorAll('.copy').forEach(function (btn) {
    btn.addEventListener('click', function () {
      var txt = btn.getAttribute('data-copy') || '';
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(txt).catch(function () { fallbackCopy(txt); });
      } else {
        fallbackCopy(txt);
      }
      var orig = btn.textContent;
      btn.textContent = 'copied!';
      btn.classList.add('copied');
      setTimeout(function () {
        btn.textContent = orig;
        btn.classList.remove('copied');
      }, 1200);
    });
  });

  function fallbackCopy(str) {
    var ta = document.createElement('textarea');
    ta.value = str;
    ta.style.position = 'fixed';
    ta.style.left = '-9999px';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); } catch (e) {}
    document.body.removeChild(ta);
  }

  /* ─── terminal click to copy (click anywhere) ─── */
  document.querySelectorAll('.term[data-copy]').forEach(function (term) {
    term.addEventListener('click', function (e) {
      if (e.target.closest('.copy')) return;
      var btn = term.querySelector('.copy');
      if (btn) btn.click();
    });
  });

  /* ─── sidebar scrollspy ─── */
  var sections = document.querySelectorAll('.section, .hero');
  var navLinks = document.querySelectorAll('.sidebar-nav a[data-section]');
  var currentSection = '';

  function updateActiveLink() {
    var scrollY = window.scrollY;
    var found = '';
    var offset = 120;

    sections.forEach(function (sec) {
      if (!sec.id) return;
      var top = sec.offsetTop;
      var height = sec.offsetHeight;
      if (scrollY >= top - offset && scrollY < top + height - offset) {
        found = sec.id;
      }
    });

    if (found !== currentSection) {
      currentSection = found;
      navLinks.forEach(function (a) {
        a.classList.toggle('active', a.getAttribute('data-section') === found);
      });
    }
  }

  window.addEventListener('scroll', updateActiveLink, { passive: true });
  window.addEventListener('resize', updateActiveLink, { passive: true });
  updateActiveLink();

  /* ─── smooth click-to-nav ─── */
  navLinks.forEach(function (a) {
    a.addEventListener('click', function (e) {
      var href = a.getAttribute('href');
      if (!href || href === '#') return;
      var target = document.querySelector(href);
      if (!target) return;

      e.preventDefault();

      var y = target.getBoundingClientRect().top + window.scrollY - 90;
      window.scrollTo({ top: y, behavior: 'smooth' });

      navLinks.forEach(function (l) { l.classList.remove('active'); });
      a.classList.add('active');
      currentSection = a.getAttribute('data-section') || '';
    });
  });

  /* ─── section anchor permalinks ─── */
  document.querySelectorAll('.section-anchor').forEach(function (a) {
    a.addEventListener('click', function (e) {
      e.preventDefault();
      var href = a.getAttribute('href');
      if (!href) return;

      if (history.pushState) {
        history.pushState(null, '', href);
      }
      var target = document.querySelector(href);
      if (target) {
        var y = target.getBoundingClientRect().top + window.scrollY - 90;
        window.scrollTo({ top: y, behavior: 'smooth' });
      }

      var url = window.location.origin + window.location.pathname + href;
      if (navigator.clipboard) {
        navigator.clipboard.writeText(url).catch(function () {});
      }
    });
  });

  /* ─── initial scroll-to-hash ─── */
  if (window.location.hash) {
    var hashTarget = document.querySelector(window.location.hash);
    if (hashTarget) {
      setTimeout(function () {
        var y = hashTarget.getBoundingClientRect().top + window.scrollY - 90;
        window.scrollTo({ top: y, behavior: 'smooth' });
      }, 200);
    }
  }

  /* ─── initial active link ─── */
  var initialHash = window.location.hash.replace('#', '');
  if (initialHash) {
    navLinks.forEach(function (a) {
      a.classList.toggle('active', a.getAttribute('data-section') === initialHash);
    });
  } else {
    var first = document.querySelector('.sidebar-nav a[data-section]');
    if (first) first.classList.add('active');
  }

})();
